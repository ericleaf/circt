//===- HandshakeToFIRRTL.cpp - Translate Handshake into FIRRTL ------------===//
//
// Copyright 2019 The CIRCT Authors.
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//

#include "circt/Conversion/HandshakeToFIRRTL/HandshakeToFIRRTL.h"
#include "circt/Dialect/FIRRTL/Ops.h"
#include "circt/Dialect/Handshake/HandshakeOps.h"
#include "circt/Dialect/StaticLogic/StaticLogic.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/Utils.h"

using namespace mlir;
using namespace circt;
using namespace circt::handshake;
using namespace circt::firrtl;

using ValueVector = llvm::SmallVector<Value, 3>;
using ValueVectorList = std::vector<ValueVector>;

//===----------------------------------------------------------------------===//
// Utils
//===----------------------------------------------------------------------===//

/// Build a FIRRTL bundle type (with data, valid, and ready subfields) given the
/// type of the data subfield.
static FIRRTLType buildBundleType(FIRRTLType dataType, bool isFlip,
                                  MLIRContext *context) {
  using BundleElement = std::pair<Identifier, FIRRTLType>;
  llvm::SmallVector<BundleElement, 3> elements;

  // Add valid and ready subfield to the bundle.
  auto validId = Identifier::get("valid", context);
  auto readyId = Identifier::get("ready", context);
  auto signalType = UIntType::get(context, 1);
  if (isFlip) {
    elements.push_back(BundleElement(validId, FlipType::get(signalType)));
    elements.push_back(BundleElement(readyId, signalType));
  } else {
    elements.push_back(BundleElement(validId, signalType));
    elements.push_back(BundleElement(readyId, FlipType::get(signalType)));
  }

  // Add data subfield to the bundle if dataType is not a null.
  if (dataType) {
    auto dataId = Identifier::get("data", context);
    if (isFlip)
      elements.push_back(BundleElement(dataId, FlipType::get(dataType)));
    else
      elements.push_back(BundleElement(dataId, dataType));
  }

  return BundleType::get(elements, context);
}

/// Return a FIRRTL bundle type (with data, valid, and ready subfields) given a
/// standard data type. Current supported data types are integer (signed,
/// unsigned, and signless), index, and none.
static FIRRTLType getBundleType(Type type, bool isFlip) {
  // If the input is already converted to a bundle type elsewhere, itself will
  // be returned after cast.
  if (auto bundleType = type.dyn_cast<BundleType>())
    return bundleType;

  MLIRContext *context = type.getContext();
  switch (type.getKind()) {
  case StandardTypes::Integer: {
    auto integerType = type.cast<IntegerType>();
    unsigned width = integerType.getWidth();

    switch (integerType.getSignedness()) {
    case IntegerType::Signed:
      return buildBundleType(SIntType::get(context, width), isFlip, context);
    case IntegerType::Unsigned:
      return buildBundleType(UIntType::get(context, width), isFlip, context);
    // ISSUE: How to handle signless integers? Should we use the AsSIntPrimOp
    // or AsUIntPrimOp to convert?
    case IntegerType::Signless:
      return buildBundleType(UIntType::get(context, width), isFlip, context);
    }
  }
  // Currently we consider index type as 64-bits unsigned integer.
  case StandardTypes::Index: {
    unsigned width = type.cast<IndexType>().kInternalStorageBitWidth;
    return buildBundleType(UIntType::get(context, width), isFlip, context);
  }
  case StandardTypes::None:
    return buildBundleType(/*dataType=*/nullptr, isFlip, context);
  default:
    return FIRRTLType(nullptr);
  }
}

static Value createConstantOp(FIRRTLType opType, APInt value,
                              Location insertLoc,
                              ConversionPatternRewriter &rewriter) {
  if (auto intOpType = opType.dyn_cast<firrtl::IntType>()) {
    auto type = rewriter.getIntegerType(intOpType.getWidthOrSentinel(),
                                        intOpType.isSigned());
    return rewriter.create<firrtl::ConstantOp>(
        insertLoc, opType, rewriter.getIntegerAttr(type, value));
  } else
    return Value(nullptr);
}

/// Construct a name for creating FIRRTL sub-module. The returned string
/// contains the following information: 1) standard or handshake operation
/// name; 2) number of inputs; 3) number of outputs; 4) comparison operation
/// type (if applied); 5) whether the elastic component is for the control path
/// (if applied).
static std::string getSubModuleName(Operation *oldOp) {
  std::string subModuleName = oldOp->getName().getStringRef().str() + "_" +
                              std::to_string(oldOp->getNumOperands()) + "ins_" +
                              std::to_string(oldOp->getNumResults()) + "outs";

  if (auto comOp = dyn_cast<mlir::CmpIOp>(oldOp))
    subModuleName += "_" + stringifyEnum(comOp.getPredicate()).str();

  if (auto bufferOp = dyn_cast<handshake::BufferOp>(oldOp)) {
    subModuleName += "_" + bufferOp.getNumSlots().toString(10, false) + "slots";
    if (bufferOp.isSequential())
      subModuleName += "_seq";
  }

  if (auto ctrlAttr = oldOp->getAttr("control")) {
    if (ctrlAttr.cast<BoolAttr>().getValue())
      subModuleName += "_ctrl";
  }

  return subModuleName;
}

//===----------------------------------------------------------------------===//
// FIRRTL Top-module Related Functions
//===----------------------------------------------------------------------===//

/// Currently we are not considering clock and registers, thus the generated
/// circuit is pure combinational logic. If graph cycle exists, at least one
/// buffer is required to be inserted for breaking the cycle, which will be
/// supported in the next patch.
static FModuleOp createTopModuleOp(handshake::FuncOp funcOp, unsigned numClocks,
                                   ConversionPatternRewriter &rewriter) {
  using ModulePort = std::pair<StringAttr, FIRRTLType>;
  llvm::SmallVector<ModulePort, 8> ports;

  // Add all inputs of funcOp.
  unsigned args_idx = 0;
  for (auto &arg : funcOp.getArguments()) {
    auto portName = rewriter.getStringAttr("arg" + std::to_string(args_idx));
    auto bundlePortType = getBundleType(arg.getType(), /*isFlip=*/false);

    if (!bundlePortType)
      funcOp.emitError("Unsupported data type. Supported data types: integer "
                       "(signed, unsigned, signless), index, none.");

    ports.push_back(ModulePort(portName, bundlePortType));
    args_idx += 1;
  }

  // Add all outputs of funcOp.
  for (auto portType : funcOp.getType().getResults()) {
    auto portName = rewriter.getStringAttr("arg" + std::to_string(args_idx));
    auto bundlePortType = getBundleType(portType, /*isFlip=*/true);

    if (!bundlePortType)
      funcOp.emitError("Unsupported data type. Supported data types: integer "
                       "(signed, unsigned, signless), index, none.");

    ports.push_back(ModulePort(portName, bundlePortType));
    args_idx += 1;
  }

  // Add clock and reset signals.
  if (numClocks == 1) {
    ports.push_back(ModulePort(rewriter.getStringAttr("clock"),
                               rewriter.getType<ClockType>()));
    ports.push_back(ModulePort(rewriter.getStringAttr("reset"),
                               rewriter.getType<UIntType>(1)));
  } else if (numClocks > 1) {
    for (unsigned i = 0; i < numClocks; ++i) {
      auto clockName = "clock" + std::to_string(i);
      auto resetName = "reset" + std::to_string(i);
      ports.push_back(ModulePort(rewriter.getStringAttr(clockName),
                                 rewriter.getType<ClockType>()));
      ports.push_back(ModulePort(rewriter.getStringAttr(resetName),
                                 rewriter.getType<UIntType>(1)));
    }
  }

  // Create a FIRRTL module, and inline the funcOp into it.
  auto topModuleOp = rewriter.create<FModuleOp>(
      funcOp.getLoc(), rewriter.getStringAttr(funcOp.getName()), ports);
  rewriter.inlineRegionBefore(funcOp.getBody(), topModuleOp.getBody(),
                              topModuleOp.end());

  // Merge the second block (inlined from funcOp) of the top-module into the
  // entry block.
  auto blockIterator = topModuleOp.getBody().begin();
  Block *entryBlock = &*blockIterator;
  Block *secondBlock = &*(++blockIterator);

  // Replace uses of each argument of the second block with the corresponding
  // argument of the entry block.
  args_idx = 0;
  for (auto &oldArg : secondBlock->getArguments()) {
    oldArg.replaceAllUsesWith(entryBlock->getArgument(args_idx));
    args_idx += 1;
  }

  // Move all operations of the second block to the entry block.
  while (!secondBlock->empty()) {
    Operation &op = secondBlock->front();
    op.moveBefore(entryBlock->getTerminator());
  }
  rewriter.eraseBlock(secondBlock);

  return topModuleOp;
}

//===----------------------------------------------------------------------===//
// FIRRTL Sub-module Related Functions
//===----------------------------------------------------------------------===//

/// Check whether a submodule with the same name has been created elsewhere.
/// Return the matched submodule if true, otherwise return nullptr.
static FModuleOp checkSubModuleOp(FModuleOp topModuleOp, Operation *oldOp) {
  for (auto &op : topModuleOp.getParentRegion()->front()) {
    if (auto subModuleOp = dyn_cast<FModuleOp>(op)) {
      if (getSubModuleName(oldOp) == subModuleOp.getName()) {
        return subModuleOp;
      }
    }
  }
  return FModuleOp(nullptr);
}

/// All standard expressions and handshake elastic components will be converted
/// to a FIRRTL sub-module and be instantiated in the top-module.
static FModuleOp
createSubModuleOp(FModuleOp topModuleOp, Operation *oldOp, bool hasClock,
                  ConversionPatternRewriter &rewriter,
                  function_ref<std::string(Operation *)> getName) {
  rewriter.setInsertionPoint(topModuleOp);
  using ModulePort = std::pair<StringAttr, FIRRTLType>;
  llvm::SmallVector<ModulePort, 8> ports;

  // Add all inputs of oldOp.
  unsigned args_idx = 0;
  for (auto portType : oldOp->getOperands().getTypes()) {
    auto portName = rewriter.getStringAttr("arg" + std::to_string(args_idx));
    auto bundlePortType = getBundleType(portType, /*isFlip=*/false);

    if (!bundlePortType)
      oldOp->emitError("Unsupported data type. Supported data types: integer "
                       "(signed, unsigned, signless), index, none.");

    ports.push_back(ModulePort(portName, bundlePortType));
    args_idx += 1;
  }

  // Add all outputs of oldOp.
  for (auto portType : oldOp->getResults().getTypes()) {
    auto portName = rewriter.getStringAttr("arg" + std::to_string(args_idx));
    auto bundlePortType = getBundleType(portType, /*isFlip=*/true);

    if (!bundlePortType)
      oldOp->emitError("Unsupported data type. Supported data types: integer "
                       "(signed, unsigned, signless), index, none.");

    ports.push_back(ModulePort(portName, bundlePortType));
    args_idx += 1;
  }

  // Add clock and reset signals.
  if (hasClock) {
    ports.push_back(ModulePort(rewriter.getStringAttr("clock"),
                               rewriter.getType<ClockType>()));
    ports.push_back(ModulePort(rewriter.getStringAttr("reset"),
                               rewriter.getType<UIntType>(1)));
  }

  return rewriter.create<FModuleOp>(
      topModuleOp.getLoc(), rewriter.getStringAttr(getName(oldOp)), ports);
}

//===----------------------------------------------------------------------===//
// Sub-module Logic Builders
//===----------------------------------------------------------------------===//

/// Extract all subfields of all ports of the sub-module.
static ValueVectorList extractSubfields(FModuleOp subModuleOp,
                                        Location insertLoc,
                                        ConversionPatternRewriter &rewriter) {
  ValueVectorList portList;
  for (auto &arg : subModuleOp.getArguments()) {
    ValueVector subfields;
    if (auto argType = arg.getType().dyn_cast<BundleType>()) {
      // Extract all subfields of all bundle ports.
      for (auto &element : argType.getElements()) {
        StringRef elementName = element.first.strref();
        FIRRTLType elementType = element.second;
        subfields.push_back(rewriter.create<SubfieldOp>(
            insertLoc, elementType, arg, rewriter.getStringAttr(elementName)));
      }
    } else if (arg.getType().isa<ClockType>() ||
               arg.getType().dyn_cast<UIntType>().getWidthOrSentinel() == 1) {
      // Extract clock and reset signals.
      subfields.push_back(arg);
    }
    portList.push_back(subfields);
  }

  return portList;
}

/// Please refer to simple_addi.mlir test case.
template <typename OpType>
static void buildBinaryLogic(ValueVectorList portList, Location insertLoc,
                             ConversionPatternRewriter &rewriter) {
  ValueVector arg0Subfield = portList[0];
  ValueVector arg1Subfield = portList[1];
  ValueVector resultSubfields = portList[2];

  Value arg0Valid = arg0Subfield[0];
  Value arg0Ready = arg0Subfield[1];
  Value arg0Data = arg0Subfield[2];
  Value arg1Valid = arg1Subfield[0];
  Value arg1Ready = arg1Subfield[1];
  Value arg1Data = arg1Subfield[2];
  Value resultValid = resultSubfields[0];
  Value resultReady = resultSubfields[1];
  Value resultData = resultSubfields[2];

  // Carry out the binary operation.
  auto resultDataOp = rewriter.create<OpType>(insertLoc, arg0Data.getType(),
                                              arg0Data, arg1Data);
  rewriter.create<ConnectOp>(insertLoc, resultData, resultDataOp);

  // Generate valid signal.
  auto resultValidOp = rewriter.create<AndPrimOp>(
      insertLoc, arg0Valid.getType(), arg0Valid, arg1Valid);
  rewriter.create<ConnectOp>(insertLoc, resultValid, resultValidOp);

  // Generate ready signals.
  auto argReadyOp = rewriter.create<AndPrimOp>(insertLoc, resultReady.getType(),
                                               resultReady, resultValidOp);
  rewriter.create<ConnectOp>(insertLoc, arg0Ready, argReadyOp);
  rewriter.create<ConnectOp>(insertLoc, arg1Ready, argReadyOp);
}

/// Please refer to test_sink.mlir test case.
static void buildSinkLogic(ValueVectorList portList, Location insertLoc,
                           ConversionPatternRewriter &rewriter) {
  ValueVector argSubfields = portList.front();
  Value argValid = argSubfields[0];
  Value argReady = argSubfields[1];
  Value argData = argSubfields[2];

  // A Sink operation is always ready to accept tokens.
  auto signalType = argValid.getType().cast<FIRRTLType>();
  Value highSignal =
      createConstantOp(signalType, APInt(1, 1), insertLoc, rewriter);
  rewriter.create<ConnectOp>(insertLoc, argReady, highSignal);

  rewriter.eraseOp(argValid.getDefiningOp());
  rewriter.eraseOp(argData.getDefiningOp());
}

/// Currently only support {control = true}.
/// Please refer to test_join.mlir test case.
static void buildJoinLogic(ValueVectorList portList, Location insertLoc,
                           ConversionPatternRewriter &rewriter) {
  ValueVector resultSubfields = portList.back();
  Value resultValid = resultSubfields[0];
  Value resultReady = resultSubfields[1];

  // The output is triggered only after all inputs are valid.
  Value *tmpValid = &portList[0][0];
  for (unsigned i = 1, e = portList.size() - 1; i < e; ++i) {
    Value argValid = portList[i][0];
    *tmpValid = rewriter.create<AndPrimOp>(insertLoc, argValid.getType(),
                                           argValid, *tmpValid);
  }
  rewriter.create<ConnectOp>(insertLoc, resultValid, *tmpValid);

  // The input will be ready to accept new token when old token is sent out.
  auto argReadyOp = rewriter.create<AndPrimOp>(insertLoc, resultReady.getType(),
                                               resultReady, *tmpValid);
  for (unsigned i = 0, e = portList.size() - 1; i < e; ++i) {
    Value argReady = portList[i][1];
    rewriter.create<ConnectOp>(insertLoc, argReady, argReadyOp);
  }
}

/// Please refer to test_mux.mlir test case.
static void buildMuxLogic(ValueVectorList portList, Location insertLoc,
                          ConversionPatternRewriter &rewriter) {
  ValueVector selectSubfields = portList.front();
  Value selectValid = selectSubfields[0];
  Value selectReady = selectSubfields[1];
  Value selectData = selectSubfields[2];
  auto selectType = selectData.getType().cast<FIRRTLType>();

  ValueVector resultSubfields = portList.back();
  Value resultValid = resultSubfields[0];
  Value resultReady = resultSubfields[1];
  Value resultData = resultSubfields[2];

  // Mux will work only when the select input is active.
  auto validWhenOp = rewriter.create<WhenOp>(insertLoc, selectValid,
                                             /*withElseRegion=*/false);
  rewriter.setInsertionPointToStart(&validWhenOp.thenRegion().front());

  // Walk through each input to create a chain of when operation.
  for (unsigned i = 1, e = portList.size() - 1; i < e; ++i) {
    ValueVector argSubfields = portList[i];
    Value argValid = argSubfields[0];
    Value argReady = argSubfields[1];
    Value argData = argSubfields[2];

    auto conditionOp = rewriter.create<EQPrimOp>(
        insertLoc, UIntType::get(rewriter.getContext(), 1), selectData,
        createConstantOp(selectType, APInt(64, i), insertLoc, rewriter));

    // If the current input is not the last one, the new created when
    // operation will have an else region.
    auto branchWhenOp = rewriter.create<WhenOp>(
        insertLoc, conditionOp, /*withElseRegion=*/(i != e - 1));

    rewriter.setInsertionPointToStart(&branchWhenOp.thenRegion().front());
    rewriter.create<ConnectOp>(insertLoc, resultValid, argValid);
    rewriter.create<ConnectOp>(insertLoc, resultData, argData);
    rewriter.create<ConnectOp>(insertLoc, argReady, resultReady);

    // Select will be ready to accept new token when data has been passed from
    // input to output.
    auto selectReadyOp = rewriter.create<AndPrimOp>(
        insertLoc, argValid.getType(), argValid, resultReady);
    rewriter.create<ConnectOp>(insertLoc, selectReady, selectReadyOp);
    if (i != e - 1)
      rewriter.setInsertionPointToStart(&branchWhenOp.elseRegion().front());
  }
}

/// Assume only one input is active. When multiple inputs are active, inputs in
/// the front have higher priority.
/// Please refer to test_merge.mlir test case.
static void buildMergeLogic(ValueVectorList portList, Location insertLoc,
                            ConversionPatternRewriter &rewriter) {
  ValueVector resultSubfields = portList.back();
  Value resultValid = resultSubfields[0];
  Value resultReady = resultSubfields[1];
  Value resultData = resultSubfields[2];

  // Walk through each input to create a chain of when operation.
  for (unsigned i = 0, e = portList.size() - 1; i < e; ++i) {
    ValueVector argSubfields = portList[i];
    Value argValid = argSubfields[0];
    Value argReady = argSubfields[1];
    Value argData = argSubfields[2];

    // If the current input is not the last one, the new created when operation
    // will have an else region.
    auto whenOp = rewriter.create<WhenOp>(insertLoc, argValid,
                                          /*withElseRegion=*/(i != e - 1));
    rewriter.setInsertionPointToStart(&whenOp.thenRegion().front());
    rewriter.create<ConnectOp>(insertLoc, resultData, argData);
    rewriter.create<ConnectOp>(insertLoc, resultValid, argValid);
    rewriter.create<ConnectOp>(insertLoc, argReady, resultReady);

    if (i != e - 1)
      rewriter.setInsertionPointToStart(&whenOp.elseRegion().front());
  }
}

/// Assume only one input is active.
/// Please refer to test_cmerge.mlir test case.
static void buildControlMergeLogic(handshake::ControlMergeOp *oldOp,
                                   ValueVectorList portList, Location insertLoc,
                                   ConversionPatternRewriter &rewriter) {
  unsigned numPorts = portList.size();

  ValueVector resultSubfields = portList[numPorts - 2];
  Value resultValid = resultSubfields[0];
  Value resultReady = resultSubfields[1];

  // The last output indicates which input is active now.
  ValueVector controlSubfields = portList[numPorts - 1];
  Value controlValid = controlSubfields[0];
  Value controlReady = controlSubfields[1];
  Value controlData = controlSubfields[2];
  auto controlType = FlipType::get(controlData.getType().cast<FIRRTLType>());

  auto argReadyOp = rewriter.create<AndPrimOp>(insertLoc, resultReady.getType(),
                                               resultReady, controlReady);

  // Walk through each input to create a chain of when operation.
  for (unsigned i = 0, e = numPorts - 2; i < e; ++i) {
    ValueVector argSubfields = portList[i];
    Value argValid = argSubfields[0];
    Value argReady = argSubfields[1];

    // If the current input is not the last one, the new created when operation
    // will have an else region.
    auto whenOp = rewriter.create<WhenOp>(insertLoc, argValid,
                                          /*withElseRegion=*/(i != e - 1));
    rewriter.setInsertionPointToStart(&whenOp.thenRegion().front());
    rewriter.create<ConnectOp>(
        insertLoc, controlData,
        createConstantOp(controlType, APInt(64, i), insertLoc, rewriter));
    rewriter.create<ConnectOp>(insertLoc, controlValid, argValid);
    rewriter.create<ConnectOp>(insertLoc, resultValid, argValid);
    rewriter.create<ConnectOp>(insertLoc, argReady, argReadyOp);

    if (!oldOp->getAttrOfType<BoolAttr>("control").getValue()) {
      Value argData = argSubfields[2];
      Value resultData = resultSubfields[2];
      rewriter.create<ConnectOp>(insertLoc, resultData, argData);
    }

    if (i != e - 1)
      rewriter.setInsertionPointToStart(&whenOp.elseRegion().front());
  }
}

/// Please refer to test_branch.mlir test case.
static void buildBranchLogic(handshake::BranchOp *oldOp,
                             ValueVectorList portList, Location insertLoc,
                             ConversionPatternRewriter &rewriter) {
  ValueVector argSubfields = portList[0];
  ValueVector resultSubfields = portList[1];
  Value argValid = argSubfields[0];
  Value argReady = argSubfields[1];
  Value resultValid = resultSubfields[0];
  Value resultReady = resultSubfields[1];

  rewriter.create<ConnectOp>(insertLoc, resultValid, argValid);
  rewriter.create<ConnectOp>(insertLoc, argReady, resultReady);

  if (!oldOp->isControl()) {
    Value argData = argSubfields[2];
    Value resultData = resultSubfields[2];
    rewriter.create<ConnectOp>(insertLoc, resultData, argData);
  }
}

/// Two outputs conditional branch operation.
/// Please refer to test_conditional_branch.mlir test case.
static void buildConditionalBranchLogic(handshake::ConditionalBranchOp *oldOp,
                                        ValueVectorList portList,
                                        Location insertLoc,
                                        ConversionPatternRewriter &rewriter) {
  ValueVector controlSubfields = portList[0];
  ValueVector argSubfields = portList[1];
  ValueVector result0Subfields = portList[2];
  ValueVector result1Subfields = portList[3];

  Value controlValid = controlSubfields[0];
  Value controlReady = controlSubfields[1];
  Value controlData = controlSubfields[2];
  Value argValid = argSubfields[0];
  Value argReady = argSubfields[1];
  Value result0Valid = result0Subfields[0];
  Value result0Ready = result0Subfields[1];
  Value result1Valid = result1Subfields[0];
  Value result1Ready = result1Subfields[1];

  // ConditionalBranch will work only when the control input is active.
  auto validWhenOp = rewriter.create<WhenOp>(insertLoc, controlValid,
                                             /*withElseRegion=*/false);
  rewriter.setInsertionPointToStart(&validWhenOp.thenRegion().front());
  auto branchWhenOp = rewriter.create<WhenOp>(insertLoc, controlData,
                                              /*withElseRegion=*/true);

  // When control signal is true, the first branch is selected
  rewriter.setInsertionPointToStart(&branchWhenOp.thenRegion().front());
  rewriter.create<ConnectOp>(insertLoc, result0Valid, argValid);
  rewriter.create<ConnectOp>(insertLoc, argReady, result0Ready);

  if (!oldOp->isControl()) {
    Value argData = argSubfields[2];
    Value result0Data = result0Subfields[2];
    rewriter.create<ConnectOp>(insertLoc, result0Data, argData);
  }

  auto control0ReadyOp = rewriter.create<AndPrimOp>(
      insertLoc, argValid.getType(), argValid, result0Ready);
  rewriter.create<ConnectOp>(insertLoc, controlReady, control0ReadyOp);

  // When control signal is false, the second branch is selected
  rewriter.setInsertionPointToStart(&branchWhenOp.elseRegion().front());
  rewriter.create<ConnectOp>(insertLoc, result1Valid, argValid);
  rewriter.create<ConnectOp>(insertLoc, argReady, result1Ready);

  if (!oldOp->isControl()) {
    Value argData = argSubfields[2];
    Value result1Data = result1Subfields[2];
    rewriter.create<ConnectOp>(insertLoc, result1Data, argData);
  }

  auto control1ReadyOp = rewriter.create<AndPrimOp>(
      insertLoc, argValid.getType(), argValid, result1Ready);
  rewriter.create<ConnectOp>(insertLoc, controlReady, control1ReadyOp);
}

/// Please refer to test_lazy_fork.mlir test case.
static void buildLazyForkLogic(handshake::LazyForkOp *oldOp,
                               ValueVectorList portList, Location insertLoc,
                               ConversionPatternRewriter &rewriter) {
  ValueVector argSubfields = portList.front();
  Value argValid = argSubfields[0];
  Value argReady = argSubfields[1];

  // The input will be ready to accept new token when all outputs are ready.
  Value *tmpReady = &portList[1][1];
  for (unsigned i = 2, e = portList.size(); i < e; ++i) {
    Value resultReady = portList[i][1];
    *tmpReady = rewriter.create<AndPrimOp>(insertLoc, resultReady.getType(),
                                           resultReady, *tmpReady);
  }
  rewriter.create<ConnectOp>(insertLoc, argReady, *tmpReady);

  // All outputs must be ready for the LazyFork to send the token.
  auto resultValidOp = rewriter.create<AndPrimOp>(insertLoc, argValid.getType(),
                                                  argValid, *tmpReady);
  for (unsigned i = 1, e = portList.size(); i < e; ++i) {
    ValueVector resultfield = portList[i];
    Value resultValid = resultfield[0];
    rewriter.create<ConnectOp>(insertLoc, resultValid, resultValidOp);

    if (!oldOp->isControl()) {
      Value argData = argSubfields[2];
      Value resultData = resultfield[2];
      rewriter.create<ConnectOp>(insertLoc, resultData, argData);
    }
  }
}

/// Currently Fork is implement as a LazyFork. An eager Fork is supposed to be
/// a timing component, and contains a register for recording which outputs
/// have accepted the token. Eager Fork will be supported in the next patch.
/// Please refer to test_lazy_fork.mlir test case.
static void buildForkLogic(handshake::ForkOp *oldOp, ValueVectorList portList,
                           Location insertLoc,
                           ConversionPatternRewriter &rewriter) {
  ValueVector argSubfields = portList.front();
  Value argValid = argSubfields[0];
  Value argReady = argSubfields[1];

  // The input will be ready to accept new token when all outputs are ready.
  Value *tmpReady = &portList[1][1];
  for (unsigned i = 2, e = portList.size(); i < e; ++i) {
    Value resultReady = portList[i][1];
    *tmpReady = rewriter.create<AndPrimOp>(insertLoc, resultReady.getType(),
                                           resultReady, *tmpReady);
  }
  rewriter.create<ConnectOp>(insertLoc, argReady, *tmpReady);

  // All outputs must be ready for the LazyFork to send the token.
  auto resultValidOp = rewriter.create<AndPrimOp>(insertLoc, argValid.getType(),
                                                  argValid, *tmpReady);
  for (unsigned i = 1, e = portList.size(); i < e; ++i) {
    ValueVector resultfield = portList[i];
    Value resultValid = resultfield[0];
    rewriter.create<ConnectOp>(insertLoc, resultValid, resultValidOp);

    if (!oldOp->isControl()) {
      Value argData = argSubfields[2];
      Value resultData = resultfield[2];
      rewriter.create<ConnectOp>(insertLoc, resultData, argData);
    }
  }
}

/// Please refer to test_constant.mlir test case.
static void buildConstantLogic(handshake::ConstantOp *oldOp,
                               ValueVectorList portList, Location insertLoc,
                               ConversionPatternRewriter &rewriter) {
  // The first input is control signal which will trigger the Constant
  // operation to emit tokens.
  ValueVector controlSubfields = portList.front();
  Value controlValid = controlSubfields[0];
  Value controlReady = controlSubfields[1];

  ValueVector resultSubfields = portList.back();
  Value resultValid = resultSubfields[0];
  Value resultReady = resultSubfields[1];
  Value resultData = resultSubfields[2];

  auto constantType = FlipType::get(resultData.getType().cast<FIRRTLType>());
  auto constantValue = oldOp->getAttrOfType<IntegerAttr>("value").getValue();

  rewriter.create<ConnectOp>(insertLoc, resultValid, controlValid);
  rewriter.create<ConnectOp>(insertLoc, controlReady, resultReady);
  rewriter.create<ConnectOp>(
      insertLoc, resultData,
      createConstantOp(constantType, constantValue, insertLoc, rewriter));
}

static void buildBufferLogic(handshake::BufferOp *oldOp,
                             ValueVectorList portList, Location insertLoc,
                             ConversionPatternRewriter &rewriter) {
  ValueVector inputSubfields = portList[0];
  Value inputValid = inputSubfields[0];
  Value inputReady = inputSubfields[1];

  ValueVector outputSubfields = portList[1];
  Value outputValid = outputSubfields[0];
  Value outputReady = outputSubfields[1];

  Value clock = portList[2][0];
  Value reset = portList[3][0];
}

//===----------------------------------------------------------------------===//
// Old Operation Conversion Functions
//===----------------------------------------------------------------------===//

/// Create InstanceOp in the top-module. This will be called after the
/// corresponding sub-module and combinational logic are created.
static void createInstOp(Operation *oldOp, FModuleOp subModuleOp,
                         FModuleOp topModuleOp, unsigned clockDomain,
                         ConversionPatternRewriter &rewriter) {
  rewriter.setInsertionPointAfter(oldOp);
  using BundleElement = std::pair<Identifier, FIRRTLType>;
  llvm::SmallVector<BundleElement, 8> elements;
  MLIRContext *context = subModuleOp.getContext();

  // Bundle all ports of the instance into a new flattened bundle type.
  unsigned args_idx = 0;
  for (auto &arg : subModuleOp.getArguments()) {
    std::string argName = "arg" + std::to_string(args_idx);
    auto argId = rewriter.getIdentifier(argName);

    // All ports of the instance operation are flipped.
    auto argType = FlipType::get(arg.getType().cast<FIRRTLType>());
    elements.push_back(BundleElement(argId, argType));
    args_idx += 1;
  }

  // Create a instance operation.
  auto instType = BundleType::get(elements, context);
  auto instanceOp = rewriter.create<firrtl::InstanceOp>(
      oldOp->getLoc(), instType, subModuleOp.getName(),
      rewriter.getStringAttr(""));

  // Connect the new created instance with its predecessors and successors in
  // the top-module.
  unsigned ports_idx = 0;
  for (auto &element : instType.cast<BundleType>().getElements()) {
    Identifier elementName = element.first;
    FIRRTLType elementType = element.second;
    auto subfieldOp = rewriter.create<SubfieldOp>(
        oldOp->getLoc(), elementType, instanceOp,
        rewriter.getStringAttr(elementName.strref()));

    unsigned numIns = oldOp->getNumOperands();
    unsigned numArgs = numIns + oldOp->getNumResults();

    auto topArgs = topModuleOp.getBody().front().getArguments();
    auto firstClock = std::find_if(topArgs.begin(), topArgs.end(),
                                   [](BlockArgument &arg) -> bool {
                                     return arg.getType().isa<ClockType>();
                                   });
    if (ports_idx < numIns) {
      // Connect input ports.
      rewriter.create<ConnectOp>(oldOp->getLoc(), subfieldOp,
                                 oldOp->getOperand(ports_idx));
    } else if (ports_idx < numArgs) {
      // Connect output ports.
      Value result = oldOp->getResult(ports_idx - numIns);
      result.replaceAllUsesWith(subfieldOp);
    } else {
      // Connect clock or reset signal.
      auto signal = *(firstClock + 2 * clockDomain + ports_idx - numArgs);
      rewriter.create<ConnectOp>(oldOp->getLoc(), subfieldOp, signal);
    }
    ports_idx += 1;
  }
  rewriter.eraseOp(oldOp);
}

static void convertReturnOp(Operation *oldOp, FModuleOp topModuleOp,
                            handshake::FuncOp funcOp,
                            ConversionPatternRewriter &rewriter) {
  rewriter.setInsertionPointAfter(oldOp);
  unsigned numIns = funcOp.getNumArguments();

  // Connect each operand of the old return operation with the corresponding
  // output ports.
  unsigned args_idx = 0;
  for (auto result : oldOp->getOperands()) {
    rewriter.create<ConnectOp>(
        oldOp->getLoc(), topModuleOp.getArgument(numIns + args_idx), result);
    args_idx += 1;
  }

  rewriter.eraseOp(oldOp);
}

/// TODO: Temporary. Need to be rewritten.
static FIRRTLType getFirrtlType(Type type) {
  if (auto firrtlType = type.dyn_cast<FIRRTLType>()) {
    return firrtlType;
  } else {
    return getBundleType(type, /*isFlip=*/false)
        .cast<firrtl::BundleType>()
        .getElement("data")
        .getValue()
        .second;
  }
}

/// Convert all standard operations in a pipeline stage to FIRRTL.
/// TODO: Add more operations support.
static void convertPipelineStages(FModuleOp subModuleOp, Location insertLoc,
                                  ConversionPatternRewriter &rewriter) {
  for (auto &block : llvm::drop_begin(subModuleOp, 1)) {
    rewriter.setInsertionPoint(block.getTerminator());
    for (auto &op : block) {
      if (auto addOp = dyn_cast<mlir::AddIOp>(op)) {

        auto firrtlAddOp = rewriter.create<firrtl::AddPrimOp>(
            insertLoc, getFirrtlType(addOp.getResult().getType()),
            addOp.getOperand(0), addOp.getOperand(1));
        addOp.getResult().replaceAllUsesWith(firrtlAddOp.getResult());
        rewriter.eraseOp(addOp);
      }
    }
  }
}

static void buildPipelineWrapper() {}

/// TODO: Add valid registers and implement flushable pipeline logic.
static void buildPipelineStructure(FModuleOp subModuleOp,
                                   ValueVectorList portList, unsigned numIns,
                                   unsigned numOuts, Location insertLoc,
                                   ConversionPatternRewriter &rewriter) {
  Value clockVal = portList[numIns + numOuts][0];
  Value resetVal = portList[numIns + numOuts + 1][0];

  // Prepare a constant zero and one operation for initializing valid registers.
  rewriter.setInsertionPoint(subModuleOp.front().getTerminator());
  auto signalType = UIntType::get(subModuleOp.getContext(), 1);
  auto valueType = rewriter.getIntegerType(1, /*isSigned=*/false);
  auto zeroConstOp = rewriter.create<firrtl::ConstantOp>(
      insertLoc, signalType, rewriter.getIntegerAttr(valueType, 0));
  auto oneConstOp = rewriter.create<firrtl::ConstantOp>(
      insertLoc, signalType, rewriter.getIntegerAttr(valueType, 1));

  // Walk through all blocks (pipeline stages), and insert required registers of
  // the pipeline structure.
  ValueVector validRegs;
  ValueVector readyWires;
  std::vector<llvm::SmallMapVector<Value, Value, 4>> dataRegs;

  unsigned blocksIdx = 0;
  for (auto &block : llvm::drop_begin(subModuleOp, 1)) {
    rewriter.setInsertionPoint(block.getTerminator());
    if (!isa<staticlogic::ReturnOp>(block.getTerminator())) {

      // PART1: Insert valid registers and ready wires for each pipeline stage.
      // Here, ready signals should not be registered otherwise the back
      // pressure will not be correctly conducted.
      auto validRegOp = rewriter.create<firrtl::RegInitOp>(
          insertLoc, signalType, clockVal, resetVal, zeroConstOp,
          rewriter.getStringAttr("valid" + std::to_string(blocksIdx)));
      validRegs.push_back(validRegOp.getResult());

      auto readyWireOp = rewriter.create<firrtl::WireOp>(
          insertLoc, signalType,
          rewriter.getStringAttr("ready" + std::to_string(blocksIdx)));
      readyWires.push_back(readyWireOp.getResult());

      // PART2: Identify values that are required to be registered, and insert
      // stage registers for these data values.
      ValueVector stageOuts;

      // Walk through all block arguments. If an argument is used by other
      // blocks, it needs to be registered.
      for (auto arg : block.getArguments()) {
        for (auto &use : arg.getUses()) {
          if (use.getOwner()->getBlock() != &block) {
            stageOuts.push_back(arg);
            break;
          }
        }
      }

      // Walk through all results of all operations in the block. If a result is
      // used by other blocks, it needs to be regitered.
      for (auto &op : block) {
        for (auto result : op.getResults()) {
          for (auto &use : result.getUses()) {
            if (use.getOwner()->getBlock() != &block) {
              // Only push back unique operands.
              if (std::find(stageOuts.begin(), stageOuts.end(), result) ==
                  stageOuts.end())
                stageOuts.push_back(result);
              break;
            }
          }
        }
      }

      // Insert data registers.
      llvm::SmallMapVector<Value, Value, 4> stageRegs;
      unsigned outsIdx = 0;
      for (auto value : stageOuts) {
        auto regOp = rewriter.create<firrtl::RegOp>(
            insertLoc, getFirrtlType(value.getType()), clockVal,
            rewriter.getStringAttr("data" + std::to_string(blocksIdx) + "." +
                                   std::to_string(outsIdx)));
        value.replaceUsesWithIf(regOp, [&block](OpOperand &use) -> bool {
          return use.getOwner()->getBlock() != &block;
        });
        stageRegs.insert(std::pair<Value, Value>(value, regOp.getResult()));
        outsIdx += 1;
      }
      dataRegs.push_back(stageRegs);

      blocksIdx += 1;
    }
  }

  // Build flushable pipeline logic.
  auto validIn = rewriter.create<firrtl::WireOp>(
      insertLoc, signalType, rewriter.getStringAttr("valid_in"));
  auto readyIn = rewriter.create<firrtl::WireOp>(
      insertLoc, signalType, rewriter.getStringAttr("ready_in"));

  for (unsigned i = 0; i < blocksIdx; ++i) {
    auto validPrev = i == 0 ? validIn : validRegs[i - 1];
    auto readyNext = i == blocksIdx - 1 ? readyIn : readyWires[i + 1];

    rewriter.setInsertionPoint(subModuleOp.back().getTerminator());
    auto whenOp = rewriter.create<firrtl::WhenOp>(insertLoc, validRegs[i],
                                                  /*withElseRegion=*/true);

    // PART1: When valid register is set high, indicating the corresponding data
    // registers are available.
    auto thenBlder = whenOp.getThenBodyBuilder();

    // Connect data registers. Only when both the valid signal from the
    // previous stage and the ready signal from the next stage are high,
    // data registers are able to be updated.
    auto dataWillUpdate = thenBlder.create<firrtl::AndPrimOp>(
        insertLoc, signalType, readyNext, validPrev);
    auto dataWhenOp =
        thenBlder.create<firrtl::WhenOp>(insertLoc, dataWillUpdate, false);
    auto dataBlder = dataWhenOp.getThenBodyBuilder();
    for (auto &pair : dataRegs[i])
      dataBlder.create<firrtl::ConnectOp>(insertLoc, pair.second, pair.first);

    // Connect valid register. Only when the valid signal from the previous
    // stage is low, and the ready signal from the next stage is high, valid
    // register will be updated to low.
    auto validWillUpdate = thenBlder.create<firrtl::AndPrimOp>(
        insertLoc, signalType, readyNext,
        thenBlder.create<firrtl::NotPrimOp>(insertLoc, signalType, validPrev));
    auto validWhenOp =
        thenBlder.create<firrtl::WhenOp>(insertLoc, validWillUpdate, false);
    auto validBlder = validWhenOp.getThenBodyBuilder();
    validBlder.create<firrtl::ConnectOp>(insertLoc, validRegs[i], zeroConstOp);

    // Connect ready wire.
    thenBlder.create<firrtl::ConnectOp>(insertLoc, readyWires[i], readyNext);

    // PART2: When valid register is set low, indicating the corresponding data
    // registers are unavailable. This case is relatively easy to understand,
    // since registers are occupied by bubbles, they are able to be updated.
    auto elseBlder = whenOp.getElseBodyBuilder();

    // Connect data registers.
    for (auto &pair : dataRegs[i])
      elseBlder.create<firrtl::ConnectOp>(insertLoc, pair.second, pair.first);

    // Connect valid and ready.
    elseBlder.create<firrtl::ConnectOp>(insertLoc, validRegs[i], validPrev);
    elseBlder.create<firrtl::ConnectOp>(insertLoc, readyWires[i], oneConstOp);
  }

  buildPipelineWrapper();
}

static void convertPipelineOp(Operation *oldOp, FModuleOp topModuleOp,
                              unsigned pipelineIdx,
                              ConversionPatternRewriter &rewriter) {
  auto subModuleOp =
      createSubModuleOp(topModuleOp, oldOp, /*hasClock=*/true, rewriter,
                        [pipelineIdx](Operation *op) -> std::string {
                          return op->getName().getStringRef().str() + "_" +
                                 std::to_string(pipelineIdx);
                        });
  Location insertLoc = subModuleOp.getLoc();
  rewriter.setInsertionPoint(subModuleOp.front().getTerminator());
  ValueVectorList portList = extractSubfields(subModuleOp, insertLoc, rewriter);

  // Inline all blocks in the pipeline operation into the FIRRTL module.
  rewriter.inlineRegionBefore(oldOp->getRegion(0), subModuleOp.getBody(),
                              subModuleOp.end());

  // Lower standard operations to FIRRTL for each pipeline stage.
  convertPipelineStages(subModuleOp, insertLoc, rewriter);

  // Build all pipeline structures.
  unsigned numIns = oldOp->getNumOperands();
  unsigned numOuts = oldOp->getNumResults();
  buildPipelineStructure(subModuleOp, portList, numIns, numOuts, insertLoc,
                         rewriter);

  // Replace all uses of arguments of the entry block with the data sub-fields
  // of arguments of the FIRRTL pipeline module.
  unsigned argsIdx = 0;
  for (auto &arg : (*(++subModuleOp.begin())).getArguments()) {
    arg.replaceAllUsesWith(portList[argsIdx][2]);
    argsIdx += 1;
  }

  // Convert return operation.
  rewriter.setInsertionPoint(subModuleOp.back().getTerminator());
  unsigned outsIdx = 0;
  for (auto result : subModuleOp.back().getTerminator()->getOperands()) {
    rewriter.create<ConnectOp>(insertLoc, portList[numIns + outsIdx][2],
                               result);
    outsIdx += 1;
  }

  // Cleanup the block structure of the pipeline FIRRTL sub-module.
  auto &entryBlock = subModuleOp.front().getOperations();
  for (auto &block :
       llvm::make_early_inc_range(llvm::drop_begin(subModuleOp, 1))) {
    rewriter.eraseOp(block.getTerminator());
    entryBlock.splice(--entryBlock.end(), block.getOperations());
    rewriter.eraseBlock(&block);
  }

  createInstOp(oldOp, subModuleOp, topModuleOp, /*clockDomain=*/0, rewriter);
}

//===----------------------------------------------------------------------===//
// HandshakeToFIRRTL lowering Pass
//===----------------------------------------------------------------------===//

/// Process of lowering:
///
/// 0)  Create and go into a new FIRRTL circuit;
/// 1)  Create and go into a new FIRRTL top-module;
/// 2)  Inline Handshake FuncOp region into the FIRRTL top-module;
/// 3)  Traverse and convert each Standard or Handshake operation:
///   i)    Check if an identical sub-module exists. If so, skip to vi);
///   ii)   Create and go into a new FIRRTL sub-module;
///   iii)  Extract data (if applied), valid, and ready subfield from each port
///         of the sub-module;
///   iv)   Build combinational logic;
///   v)    Exit the sub-module and go back to the top-module;
///   vi)   Create an new instance for the sub-module;
///   vii)  Connect the instance with its predecessors and successors;
/// 4)  Erase the Handshake FuncOp.
///
/// createTopModuleOp():  1) and 2)
/// checkSubModuleOp():   3.i)
/// createSubModuleOp():  3.ii)
/// extractSubfields():   3.iii)
/// build*Logic():        3.iv)
/// createInstOp():       3.v), 3.vi), and 3.vii)
///
/// Please refer to test_addi.mlir test case.
struct HandshakeFuncOpLowering : public OpConversionPattern<handshake::FuncOp> {
  using OpConversionPattern<handshake::FuncOp>::OpConversionPattern;

  LogicalResult match(Operation *op) const override { return success(); }

  void rewrite(handshake::FuncOp funcOp, ArrayRef<Value> operands,
               ConversionPatternRewriter &rewriter) const override {
    // Create FIRRTL circuit and top-module operation.
    auto circuitOp = rewriter.create<CircuitOp>(
        funcOp.getLoc(), rewriter.getStringAttr(funcOp.getName()));
    rewriter.setInsertionPointToStart(circuitOp.getBody());
    auto topModuleOp = createTopModuleOp(funcOp, /*numClocks=*/1, rewriter);
    unsigned pipelineIdx = 0;

    // Traverse and convert each operation in funcOp.
    for (Operation &op : topModuleOp.getBody().front()) {
      if (isa<handshake::ReturnOp>(op))
        convertReturnOp(&op, topModuleOp, funcOp, rewriter);

      // Convert static scheduled pipeline operations to a FIRRTL sub-module.
      else if (isa<staticlogic::PipelineOp>(op)) {
        convertPipelineOp(&op, topModuleOp, pipelineIdx, rewriter);
        pipelineIdx += 1;
      }

      // This branch takes care of all other standard and hanshake operations
      // that require to be instantiated in the top-module.
      else if (op.getDialect()->getNamespace() == "std" ||
               op.getDialect()->getNamespace() == "handshake") {
        FModuleOp subModuleOp = checkSubModuleOp(topModuleOp, &op);
        bool hasClock = isa<handshake::BufferOp>(op);

        // Check if the sub-module already exists.
        if (!subModuleOp) {
          subModuleOp = createSubModuleOp(topModuleOp, &op, hasClock, rewriter,
                                          getSubModuleName);

          Operation *termOp = subModuleOp.getBody().front().getTerminator();
          Location insertLoc = termOp->getLoc();
          rewriter.setInsertionPoint(termOp);

          ValueVectorList portList =
              extractSubfields(subModuleOp, insertLoc, rewriter);

          // Build standard expressions logic.
          if (isa<mlir::AddIOp>(op))
            buildBinaryLogic<AddPrimOp>(portList, insertLoc, rewriter);

          else if (isa<mlir::SubIOp>(op))
            buildBinaryLogic<SubPrimOp>(portList, insertLoc, rewriter);

          else if (isa<mlir::MulIOp>(op))
            buildBinaryLogic<MulPrimOp>(portList, insertLoc, rewriter);

          else if (isa<mlir::AndOp>(op))
            buildBinaryLogic<AndPrimOp>(portList, insertLoc, rewriter);

          else if (isa<mlir::OrOp>(op))
            buildBinaryLogic<OrPrimOp>(portList, insertLoc, rewriter);

          else if (isa<mlir::XOrOp>(op))
            buildBinaryLogic<XorPrimOp>(portList, insertLoc, rewriter);

          else if (auto cmpOp = dyn_cast<mlir::CmpIOp>(op)) {
            auto cmpOpAttr = cmpOp.getPredicate();
            switch (cmpOpAttr) {
            case CmpIPredicate::eq:
              buildBinaryLogic<EQPrimOp>(portList, insertLoc, rewriter);
              break;
            case CmpIPredicate::ne:
              buildBinaryLogic<NEQPrimOp>(portList, insertLoc, rewriter);
              break;
            case CmpIPredicate::slt:
              buildBinaryLogic<LTPrimOp>(portList, insertLoc, rewriter);
              break;
            case CmpIPredicate::sle:
              buildBinaryLogic<LEQPrimOp>(portList, insertLoc, rewriter);
              break;
            case CmpIPredicate::sgt:
              buildBinaryLogic<GTPrimOp>(portList, insertLoc, rewriter);
              break;
            case CmpIPredicate::sge:
              buildBinaryLogic<GEQPrimOp>(portList, insertLoc, rewriter);
              break;
            }
          } else if (isa<mlir::ShiftLeftOp>(op))
            buildBinaryLogic<DShlPrimOp>(portList, insertLoc, rewriter);

          else if (isa<mlir::SignedShiftRightOp>(op))
            buildBinaryLogic<DShrPrimOp>(portList, insertLoc, rewriter);

          // Build handshake elastic components logic.
          else if (isa<handshake::SinkOp>(op))
            buildSinkLogic(portList, insertLoc, rewriter);

          else if (isa<handshake::JoinOp>(op))
            buildJoinLogic(portList, insertLoc, rewriter);

          else if (isa<handshake::MuxOp>(op))
            buildMuxLogic(portList, insertLoc, rewriter);

          else if (isa<handshake::MergeOp>(op))
            buildMergeLogic(portList, insertLoc, rewriter);

          else if (auto oldOp = dyn_cast<handshake::ControlMergeOp>(op))
            buildControlMergeLogic(&oldOp, portList, insertLoc, rewriter);

          else if (auto oldOp = dyn_cast<handshake::BranchOp>(op))
            buildBranchLogic(&oldOp, portList, insertLoc, rewriter);

          else if (auto oldOp = dyn_cast<handshake::ConditionalBranchOp>(op))
            buildConditionalBranchLogic(&oldOp, portList, insertLoc, rewriter);

          else if (auto oldOp = dyn_cast<handshake::ForkOp>(op))
            buildForkLogic(&oldOp, portList, insertLoc, rewriter);

          else if (auto oldOp = dyn_cast<handshake::LazyForkOp>(op))
            buildLazyForkLogic(&oldOp, portList, insertLoc, rewriter);

          else if (auto oldOp = dyn_cast<handshake::ConstantOp>(op))
            buildConstantLogic(&oldOp, portList, insertLoc, rewriter);

          else if (auto oldOp = dyn_cast<handshake::BufferOp>(op))
            buildBufferLogic(&oldOp, portList, insertLoc, rewriter);

          else
            oldOp.emitError("Usupported operation type.");
        }

        // Instantiate the new created sub-module.
        createInstOp(&op, subModuleOp, topModuleOp, /*clockDomain=*/0,
                     rewriter);
      }
    }
    rewriter.eraseOp(funcOp);
  }
};

namespace {
class HandshakeToFIRRTLPass
    : public mlir::PassWrapper<HandshakeToFIRRTLPass,
                               OperationPass<handshake::FuncOp>> {
public:
  void runOnOperation() override {
    auto op = getOperation();

    ConversionTarget target(getContext());
    target.addLegalDialect<FIRRTLDialect>();
    target.addIllegalDialect<handshake::HandshakeOpsDialect>();

    OwningRewritePatternList patterns;
    patterns.insert<HandshakeFuncOpLowering>(op.getContext());

    if (failed(applyPartialConversion(op, target, patterns)))
      signalPassFailure();
  }
};
} // end anonymous namespace

void handshake::registerHandshakeToFIRRTLPasses() {
  PassRegistration<HandshakeToFIRRTLPass>("lower-handshake-to-firrtl",
                                          "Lowering to FIRRTL Dialect");
}