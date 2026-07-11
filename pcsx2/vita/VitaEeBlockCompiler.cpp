// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaEeBlockCompiler.h"
#include "pcsx2/COP0.h"
#include "pcsx2/MemoryTypes.h"
#include "pcsx2/vtlb.h"

#if defined(VITASX2_QEMU_VALIDATION) && !defined(VITASX2_QEMU_FULL_CORE)
#define VITASX2_QEMU_PROVIDER_FIXTURE 1
#endif

#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
#include "vita/qemu/VitaEeQemuStubs.h"
#else
#include "pcsx2/Config.h"
#include "pcsx2/DebugTools/GsTrace.h"
#include "pcsx2/DebugTools/VuTrace.h"
#include "pcsx2/Memory.h"
#include "pcsx2/R5900.h"
#include "pcsx2/R5900OpcodeTables.h"
#include "pcsx2/VUmicro.h"
#endif
#include "pcsx2/vita/A32Emitter.h"
#if !defined(VITASX2_QEMU_PROVIDER_FIXTURE)
#include "pcsx2/vita/VitaCore.h"
#include "pcsx2/DebugTools/GsTrace.h"

#include "common/Console.h"
#include "fmt/format.h"
#endif

#include <algorithm>
#include <cstddef>
#if defined(VITASX2_QEMU_VALIDATION)
#include <cstdio>
#endif
#include <string>
#include <type_traits>

#if !defined(VITASX2_QEMU_PROVIDER_FIXTURE)
extern void vu0Sync();
extern void _vu0FinishMicro();
extern void _vu0WaitMicro();
#endif
void executeCacheOp(u32 op, u32 addr);

#if defined(VITASX2_QEMU_VALIDATION)
u32 g_qemuDivSignedHelperCalls = 0;
u32 g_qemuDivUnsignedHelperCalls = 0;
u32 g_qemuDivSigned1HelperCalls = 0;
u32 g_qemuDivUnsigned1HelperCalls = 0;
u32 g_qemuPackedDivSignedWordHelperCalls = 0;
u32 g_qemuPackedDivUnsignedWordHelperCalls = 0;
u32 g_qemuPackedDivWordByHalfwordHelperCalls = 0;
u32 g_qemuByteMemoryHelperCalls = 0;
u32 g_qemuHalfwordMemoryHelperCalls = 0;
u32 g_qemuPartialWordMemoryHelperCalls = 0;
u32 g_qemuWordMemoryHelperCalls = 0;
u32 g_qemuDwordMemoryHelperCalls = 0;
u32 g_qemuPartialDwordMemoryHelperCalls = 0;
u32 g_qemuCop1MemoryHelperCalls = 0;
u32 g_qemuQwordGprMemoryHelperCalls = 0;
u32 g_qemuQwordCop2MemoryHelperCalls = 0;
u32 g_qemuCpuCancelInstructionCalls = 0;
u32 g_qemuVu0SyncCalls = 0;
u32 g_qemuVu0FinishMicroCalls = 0;
u32 g_qemuVu0WaitMicroCalls = 0;
u32 g_qemuVu1FinishCalls = 0;
u32 g_qemuVu1FinishAddCyclesCalls = 0;
u32 g_qemuVu1ExecMicroCalls = 0;
u32 g_qemuVu1ExecMicroLastAddr = 0;
u32 g_qemuGprPinnedBlocks = 0;
u32 g_qemuGprPinnedRegisters = 0;
u32 g_qemuGprPinnedDwordRegisters = 0;
u32 g_qemuGprPinnedVtlbFreeHostRegisters = 0;
u32 g_qemuGprPinnedHighWordHits = 0;
u32 g_qemuGprPinSelfStoresElided = 0;
u32 g_qemuGprPinHighReadOperands = 0;
u32 g_qemuGprPinExtendedHighStoreOperands = 0;
u32 g_qemuGprPinLowResultStoreOperands = 0;
u32 g_qemuGprPinEntryLoadInstructionsElided = 0;
u32 g_qemuGprDirtyPinBlocks = 0;
u32 g_qemuGprDirtyPinLowStoresElided = 0;
u32 g_qemuGprDirtyPinHighStoresElided = 0;
u32 g_qemuGprDirtyPinFlushStores = 0;
u32 g_qemuGprPinColdSyncWordsElided = 0;
u32 g_qemuGprPinColdSyncWordsStored = 0;
u32 g_qemuGpr64BackingLoadInstructions = 0;
u32 g_qemuGprHighBackingLoadInstructions = 0;
u32 g_qemuGpr64BackingStoreInstructions = 0;
u32 g_qemuCallerSavedBranchFlagBlocks = 0;
u32 g_qemuForwardedBooleanBranchBlocks = 0;
u32 g_qemuForwardedBooleanBranchLoadsElided = 0;
u32 g_qemuForwardedBooleanBranchStoresElided = 0;
u32 g_qemuForwardedBooleanBranchNormalizationsElided = 0;
u32 g_qemuForwardedBooleanBranchSyncWords = 0;
u32 g_qemuResidentForwardedBooleanHighZeroHotInstructionsElided = 0;
u32 g_qemuResidentForwardedBooleanHighZeroTranslationInstructions = 0;
u32 g_qemuResidentForwardedBooleanHighZeroHandlerInstructions = 0;
u32 g_qemuResidentForwardedBooleanMaskHotInstructionsElided = 0;
u32 g_qemuResidentForwardedBooleanMaskCanonicalInstructions = 0;
u32 g_qemuResidentForwardedBooleanMaskPageInstructions = 0;
u32 g_qemuResidentForwardedBooleanMaskHandlerInstructions = 0;
u32 g_qemuResidentForwardedBooleanMaskPublicationInstructions = 0;
u32 g_qemuResidentUnsignedBranchSuffixBlocks = 0;
u32 g_qemuResidentUnsignedBranchSuffixHotInstructionsElided = 0;
u32 g_qemuResidentUnsignedBranchSuffixEventInstructions = 0;
u32 g_qemuResidentRawGpr0QwordBlocks = 0;
u32 g_qemuResidentRawGpr0QwordStoreSelections = 0;
u32 g_qemuResidentRawGpr0QwordHotInstructionsElided = 0;
u32 g_qemuResidentRawGpr0QwordColdReloadInstructions = 0;
u32 g_qemuResidentVtlbQwordPointerBlocks = 0;
u32 g_qemuResidentVtlbQwordPointerStores = 0;
u32 g_qemuResidentVtlbQwordPointerGuardInstructions = 0;
u32 g_qemuResidentVtlbQwordPointerTranslationInstructions = 0;
u32 g_qemuResidentVtlbQwordPointerHotInstructionsElided = 0;
u32 g_qemuResidentVtlbQwordPointerColdInvalidationInstructions = 0;
u32 g_qemuResidentVtlbQwordPointerPostIncrementStores = 0;
u32 g_qemuCompatibleVtlbPointerBlocks = 0;
u32 g_qemuCompatibleVtlbPointerCanonicalPoisonInstructions = 0;
u32 g_qemuCompatibleVtlbPointerGuardInstructions = 0;
u32 g_qemuCompatibleVtlbPointerTranslationInstructions = 0;
u32 g_qemuCompatibleVtlbPointerHotInstructionsElided = 0;
u32 g_qemuCompatibleVtlbPointerPostIncrementLoads = 0;
u32 g_qemuCompatibleVtlbPointerColdInvalidationInstructions = 0;
u32 g_qemuResidentCycleLowBlocks = 0;
u32 g_qemuResidentCycleLowHotInstructionsElided = 0;
u32 g_qemuResidentCycleLowSyncInstructions = 0;
u32 g_qemuResidentCycleLowWrapFixupInstructions = 0;
u32 g_qemuResidentCycleLowColdReloadInstructions = 0;
u32 g_qemuResidentNextEventLowBlocks = 0;
u32 g_qemuResidentNextEventLowHotInstructionsElided = 0;
u32 g_qemuResidentNextEventLowTranslationReloadInstructions = 0;
u32 g_qemuResidentNextEventLowColdReloadInstructions = 0;
u32 g_qemuResidentSchedulerCountdownBlocks = 0;
u32 g_qemuResidentSchedulerCountdownHotInstructionsElided = 0;
u32 g_qemuResidentSchedulerCountdownCanonicalInstructions = 0;
u32 g_qemuResidentSchedulerCountdownPageCarryInstructions = 0;
u32 g_qemuResidentSchedulerCountdownHandlerInstructions = 0;
u32 g_qemuGprConstBlocks = 0;
u32 g_qemuGprConstResultStores = 0;
u32 g_qemuGprConstStoreValueFastPaths = 0;
u32 g_qemuGprConstPinnedStoreOperands = 0;
u32 g_qemuGprConstHighWordLoadFastPaths = 0;
u32 g_qemuGprConstRegisterJumpTargets = 0;
u32 g_qemuGprConstEffectiveAddresses = 0;
u32 g_qemuPersistentVtlbResidentBlocks = 0;
u32 g_qemuWaitLoopFastForwardBlocks = 0;
u32 g_qemuDeferredPcWritebackBlocks = 0;
u32 g_qemuDeferredIndirectPcWritebackBlocks = 0;
u32 g_qemuLinkedPcSyncBlocks = 0;
u32 g_qemuScalarZeroLoadSkips = 0;
u32 g_qemuPartialZeroLoadSkips = 0;
u32 g_qemuCop2QwordZeroLoadSkips = 0;
u32 g_qemuCop2QwordZeroStoreFastPaths = 0;
u32 g_qemuCop2Vf0ConstantTransferFastPaths = 0;
u32 g_qemuCop2RawGpr0Qmtc2ZeroFastPaths = 0;
u32 g_qemuCop2Qmtc2QCacheFastPaths = 0;
u32 g_qemuCop2Qmtc2QCacheDirectStores = 0;
u32 g_qemuCop2ControlKnownSourceFastPaths = 0;
u32 g_qemuCop0KnownSourceFastPaths = 0;
u32 g_qemuCop1KnownSourceFastPaths = 0;
u32 g_qemuVu0BaseRegisterBlocks = 0;
u32 g_qemuVu0ClipflagBaseAddressFastPaths = 0;
u32 g_qemuGprPartialStoreValueFastPaths = 0;
u32 g_qemuPartialWordFullLoadFastPaths = 0;
u32 g_qemuPartialWordFullStoreFastPaths = 0;
u32 g_qemuGprPartialDwordStoreValueFastPaths = 0;
u32 g_qemuPartialDwordFullLoadFastPaths = 0;
u32 g_qemuPartialDwordMergedLoadFastPaths = 0;
u32 g_qemuPartialDwordFullStoreFastPaths = 0;
u32 g_qemuKnownVtlbScalarFastPaths = 0;
u32 g_qemuKnownVtlbQwordFastPaths = 0;
u32 g_qemuKnownVtlbCop1FastPaths = 0;
u32 g_qemuKnownVtlbCop2FastPaths = 0;
u32 g_qemuKnownVtlbPartialFastPaths = 0;
u32 g_qemuKnownVtlbRegisterlessBlocks = 0;
u32 g_qemuCop1NormalizedOperandSkips = 0;
u32 g_qemuGprQCacheBlocks = 0;
u32 g_qemuGprQCacheEntryLoads = 0;
u32 g_qemuGprQCacheHits = 0;
u32 g_qemuGprQCacheMisses = 0;
u32 g_qemuGprQCacheWordHits = 0;
u32 g_qemuGprQCacheDwordHits = 0;
u32 g_qemuGprQCacheDirectCopyStores = 0;
u32 g_qemuGprQCacheDirectPreservedCopyStores = 0;
u32 g_qemuGprQCacheDirectPreservedMutatingSourceCopies = 0;
u32 g_qemuGprQCacheDirectInvertStores = 0;
u32 g_qemuGprQCacheDirectHiLoStores = 0;
u32 g_qemuGprQCacheDirectTransformStores = 0;
u32 g_qemuGprQCacheDirectMemoryStores = 0;
u32 g_qemuGprQCacheDirectBinaryOps = 0;
u32 g_qemuGprQCacheDirectInPlaceBinaryOps = 0;
u32 g_qemuGprQCacheDirectMixedBinaryOps = 0;
u32 g_qemuGprQCacheDirectZeroBinaryOps = 0;
u32 g_qemuGprQCacheDirectZeroResultReuses = 0;
u32 g_qemuGprQCacheDirectPackEvenOps = 0;
u32 g_qemuGprQCacheDirectHalfwordShuffleOps = 0;
u32 g_qemuGprQCacheDirectMixedHalfwordShuffleOps = 0;
u32 g_qemuGprQCacheDirectWordShuffleOps = 0;
u32 g_qemuGprQCacheDirectInterleaveOps = 0;
u32 g_qemuGprQCacheDirectDwordPairOps = 0;
u32 g_qemuGprQCacheDirectPadsbhOps = 0;
u32 g_qemuGprQCacheDirectFiveBitOps = 0;
u32 g_qemuGprQCacheDirectVariableShiftOps = 0;
u32 g_qemuGprQCacheDirectWordDivideZeroDivisorOps = 0;
u32 g_qemuGprQCacheDirectWordDivideZeroDividendOps = 0;
u32 g_qemuGprQCacheDirectWordDividePowerOfTwoOps = 0;
u32 g_qemuGprQCacheDirectWordByHalfwordDivideOps = 0;
u32 g_qemuGprQCacheDirectWordMultiplyOps = 0;
u32 g_qemuGprQCacheDirectWordMultiplyAddOps = 0;
u32 g_qemuGprQCacheDirectHalfwordMultiplyOps = 0;
u32 g_qemuGprQCacheDirectHalfwordAccumulateOps = 0;
u32 g_qemuGprQCacheDirectHalfwordPairMultiplyOps = 0;
u32 g_qemuGprQCacheDirectLow64CopyStores = 0;
u32 g_qemuGprQCacheDirectLow64PinCopyStores = 0;
u32 g_qemuGprQCacheSameSourceQregReuses = 0;
u32 g_qemuGprQCacheSingleUseEntryQwordLoads = 0;
u32 g_qemuGprQCacheMissMmiMultDiv = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordMultiply = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordMultiplyAdd = 0;
u32 g_qemuGprQCacheMissMmiMultDivHalfwordMultiply = 0;
u32 g_qemuGprQCacheMissMmiMultDivHalfwordAccumulate = 0;
u32 g_qemuGprQCacheMissMmiMultDivHalfwordPairMultiply = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordDivide = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordDivideRs = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordDivideRt = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordDivideEntry = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordDivideDefined = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordDivideEntryReadOnce = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordDivideEntryReadTwice = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordDivideEntryReadThreePlus = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivide = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivideRs = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivideRt = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivideEntry = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivideDefined = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivideEntryReadOnce = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivideEntryReadTwice = 0;
u32 g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivideEntryReadThreePlus = 0;
u32 g_qemuGprQCacheMissMmiMultDivOther = 0;
u32 g_qemuGprQCacheMissMmiVector = 0;
u32 g_qemuGprQCacheMissMmiVectorWrap = 0;
u32 g_qemuGprQCacheMissMmiVectorCompare = 0;
u32 g_qemuGprQCacheMissMmiVectorSignedSaturating = 0;
u32 g_qemuGprQCacheMissMmiVectorUnsignedSaturating = 0;
u32 g_qemuGprQCacheMissMmiVectorLogical = 0;
u32 g_qemuGprQCacheMissMmiVectorLogicalPand = 0;
u32 g_qemuGprQCacheMissMmiVectorLogicalPxor = 0;
u32 g_qemuGprQCacheMissMmiVectorLogicalPor = 0;
u32 g_qemuGprQCacheMissMmiVectorLogicalPorRs = 0;
u32 g_qemuGprQCacheMissMmiVectorLogicalPorRt = 0;
u32 g_qemuGprQCacheMissMmiVectorLogicalPorEntry = 0;
u32 g_qemuGprQCacheMissMmiVectorLogicalPorDefined = 0;
u32 g_qemuGprQCacheMissMmiVectorLogicalPorEntryReadOnce = 0;
u32 g_qemuGprQCacheMissMmiVectorLogicalPorEntryReadTwice = 0;
u32 g_qemuGprQCacheMissMmiVectorLogicalPorEntryReadThreePlus = 0;
u32 g_qemuGprQCacheMissMmiVectorLogicalPnor = 0;
u32 g_qemuGprQCacheMissMmiVectorUnary = 0;
u32 g_qemuGprQCacheMissMmiVectorUnaryRt = 0;
u32 g_qemuGprQCacheMissMmiVectorUnaryEntry = 0;
u32 g_qemuGprQCacheMissMmiVectorUnaryDefined = 0;
u32 g_qemuGprQCacheMissMmiVectorUnaryEntryReadOnce = 0;
u32 g_qemuGprQCacheMissMmiVectorUnaryEntryReadTwice = 0;
u32 g_qemuGprQCacheMissMmiVectorUnaryEntryReadThreePlus = 0;
u32 g_qemuGprQCacheMissMmiVectorImmediateShift = 0;
u32 g_qemuGprQCacheMissMmiVectorImmediateShiftPsllh = 0;
u32 g_qemuGprQCacheMissMmiVectorImmediateShiftPsrlh = 0;
u32 g_qemuGprQCacheMissMmiVectorImmediateShiftPsrah = 0;
u32 g_qemuGprQCacheMissMmiVectorImmediateShiftPsllw = 0;
u32 g_qemuGprQCacheMissMmiVectorImmediateShiftPsrlw = 0;
u32 g_qemuGprQCacheMissMmiVectorImmediateShiftPsraw = 0;
u32 g_qemuGprQCacheMissMmiVectorImmediateShiftOtherOp = 0;
u32 g_qemuGprQCacheMissMmiVectorImmediateShiftRt = 0;
u32 g_qemuGprQCacheMissMmiVectorImmediateShiftEntry = 0;
u32 g_qemuGprQCacheMissMmiVectorImmediateShiftDefined = 0;
u32 g_qemuGprQCacheMissMmiVectorImmediateShiftEntryReadOnce = 0;
u32 g_qemuGprQCacheMissMmiVectorImmediateShiftEntryReadTwice = 0;
u32 g_qemuGprQCacheMissMmiVectorImmediateShiftEntryReadThreePlus = 0;
u32 g_qemuGprQCacheMissMmiVectorVariableShift = 0;
u32 g_qemuGprQCacheMissMmiVectorVariableShiftRs = 0;
u32 g_qemuGprQCacheMissMmiVectorVariableShiftRt = 0;
u32 g_qemuGprQCacheMissMmiVectorVariableShiftEntry = 0;
u32 g_qemuGprQCacheMissMmiVectorVariableShiftDefined = 0;
u32 g_qemuGprQCacheMissMmiVectorVariableShiftEntryReadOnce = 0;
u32 g_qemuGprQCacheMissMmiVectorVariableShiftEntryReadTwice = 0;
u32 g_qemuGprQCacheMissMmiVectorVariableShiftEntryReadThreePlus = 0;
u32 g_qemuGprQCacheMissMmiVectorOther = 0;
u32 g_qemuGprQCacheMissMmiShuffle = 0;
u32 g_qemuGprQCacheMissMmiHiLo = 0;
u32 g_qemuGprQCacheMissMmiInterleave = 0;
u32 g_qemuGprQCacheMissMmiPackEven = 0;
u32 g_qemuGprQCacheMissMmiPackEvenRs = 0;
u32 g_qemuGprQCacheMissMmiPackEvenRt = 0;
u32 g_qemuGprQCacheMissMmiPackEvenEntry = 0;
u32 g_qemuGprQCacheMissMmiPackEvenDefined = 0;
u32 g_qemuGprQCacheMissMmiPackEvenEntryReadOnce = 0;
u32 g_qemuGprQCacheMissMmiPackEvenEntryReadTwice = 0;
u32 g_qemuGprQCacheMissMmiPackEvenEntryReadThreePlus = 0;
u32 g_qemuGprQCacheMissMmiFiveBit = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShuffle = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShufflePinth = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShufflePexeh = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShufflePrevh = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShufflePinteh = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShufflePexch = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShufflePcpyh = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShuffleOtherOp = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShuffleRs = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShuffleRt = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShuffleEntry = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShuffleDefined = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShuffleEntryReadOnce = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShuffleEntryReadTwice = 0;
u32 g_qemuGprQCacheMissMmiHalfwordShuffleEntryReadThreePlus = 0;
u32 g_qemuGprQCacheMissMmiWordShuffle = 0;
u32 g_qemuGprQCacheMissMmiDwordPair = 0;
u32 g_qemuGprQCacheMissMmiQfsrv = 0;
u32 g_qemuGprQCacheMissMmiShuffleOther = 0;
u32 g_qemuGprQCacheMissCop2 = 0;
u32 g_qemuGprQCacheMissQwordMemory = 0;
u32 g_qemuGprQCacheMissOther = 0;
u32 g_qemuMmiFiveBitVectorOps = 0;
u32 g_qemuMmiVariableWordShiftVectorOps = 0;
u32 g_qemuMmiPackedWordMultiplyVectorOps = 0;
u32 g_qemuMmiPackedWordMultiplyAddVectorOps = 0;
u32 g_qemuMmiPackedWordDivideVectorOps = 0;
u32 g_qemuMmiPackedWordDivideKnownWordLoads = 0;
u32 g_qemuMmiPackedWordDivideZeroDivisorVectorOps = 0;
u32 g_qemuMmiPackedWordDivideZeroDividendVectorOps = 0;
u32 g_qemuMmiPackedHalfwordMultiplyAccumulateVectorOps = 0;
u32 g_qemuMmiPackedHalfwordPairMultiplyVectorOps = 0;
u32 g_qemuMmiPackedHalfwordMultiplyVectorOps = 0;
u32 g_qemuMmiPackedWordByHalfwordDivideVectorOps = 0;
u32 g_qemuMmiPackedWordByHalfwordDivideKnownDivisorFastPaths = 0;
u32 g_qemuMmiPackedWordByHalfwordDivideKnownArbitraryDivisors = 0;
u32 g_qemuMmiPackedWordByHalfwordDivideZeroDividendVectorOps = 0;
u32 g_qemuMmiPackedWordByHalfwordDivideSharedDivisorReloadsElided = 0;
u32 g_qemuMmiQfsrvKnownSaVectorOps = 0;
u32 g_qemuMmiQfsrvKnownSaZeroCopies = 0;
u32 g_qemuMmiQfsrvKnownSaZeroCopyQCacheOps = 0;
u32 g_qemuMmiQfsrvKnownSaQCacheOps = 0;
u32 g_qemuMmiHalfwordShuffleVectorOps = 0;
u32 g_qemuMmiHalfwordShuffleZeroSourceOps = 0;
u32 g_qemuMmiWordShuffleVectorOps = 0;
u32 g_qemuMmiImmediateShiftZeroSourceOps = 0;
u32 g_qemuMmiPackEvenZeroSourceOps = 0;
u32 g_qemuSigned64CompareCarryChains = 0;
u32 g_qemuUnsignedKnown64CompareCarryChains = 0;
u32 g_qemuAndLowMaskBitfieldFastPaths = 0;
u32 g_qemuNegativeHighCarryFastPaths = 0;
u32 g_qemuReverseSubtractCarryImmediateFastPaths = 0;
u32 g_qemuCarryModifiedImmediateFastPaths = 0;
u32 g_qemuShift64FusedMergeFastPaths = 0;
u32 g_qemuKnownVariableShiftImmediateFastPaths = 0;
u32 g_qemuInverseFlagImmediateFastPaths = 0;
u32 g_qemuInverseCarryImmediateFastPaths = 0;
u32 g_qemuEorAllOnesFastPaths = 0;
u32 g_qemuConditionalMovePredicatedRegisterCopies = 0;
u32 g_qemuConditionalMovePredicatedKnownCopies = 0;
u32 g_qemuConditionalMovePredicatedBackingCopies = 0;
u32 g_qemuConditionalMovePredicatedMixedCopies = 0;
u32 g_qemuConditionalMovePredicatedQCacheCopies = 0;
u32 g_qemuConditionalMovePredicatedRegisterStores = 0;
u32 g_qemuConditionalMovePredicatedQCacheStores = 0;
u32 g_qemuConditionalMoveUnconditionalQCacheCopies = 0;
u32 g_qemuSignedBranchSignBitFastPaths = 0;
u32 g_qemuSignedBranchOneCompareFastPaths = 0;
#endif

namespace VitaEE
{
	namespace
	{
		constexpr u16 REG_R3 = 1u << 3;
		constexpr u16 REG_R4 = 1u << 4;
		constexpr u16 REG_R5 = 1u << 5;
		constexpr u16 REG_R6 = 1u << 6;
		constexpr u16 REG_R7 = 1u << 7;
		constexpr u16 REG_R8 = 1u << 8;
		constexpr u16 REG_R9 = 1u << 9;
		constexpr u16 REG_R10 = 1u << 10;
		constexpr u16 REG_R11 = 1u << 11;
		constexpr u16 REG_LR = 1u << 14;
		constexpr u16 REG_PC = 1u << 15;
		constexpr u16 EE_LINK_FRAME_REGISTERS = BlockCompiler::LINK_FRAME_REGISTER_MASK;
		static_assert(EE_LINK_FRAME_REGISTERS ==
			(REG_R3 | REG_R4 | REG_R5 | REG_R6 | REG_R7 | REG_R8 | REG_R9 | REG_R10 | REG_R11));
		// Must match VitaEE::BlockExitKind without including the executor.
		constexpr u8 EE_DIRECT_EXIT_TOKEN = 0xd1;
		constexpr u8 EE_EVENT_EXIT_TOKEN = 0xe7;

		constexpr unsigned HOST_CPU_REGS = 4;
		constexpr unsigned HOST_BRANCH_STATE = 5;
		constexpr unsigned HOST_BRANCH_FLAG = HOST_BRANCH_STATE;
		constexpr unsigned HOST_BRANCH_TARGET = HOST_BRANCH_STATE;
		constexpr unsigned HOST_SP = 13;
		constexpr unsigned HOST_LR = 14;
		constexpr unsigned HOST_TMP0 = 0;
		constexpr unsigned HOST_TMP1 = 1;
		constexpr unsigned HOST_TMP2 = 2;
		constexpr unsigned HOST_TMP3 = 3;
		constexpr unsigned HOST_TMP4 = 12;
		constexpr unsigned HOST_CALLER_SAVED_BRANCH_FLAG = HOST_TMP4;
		constexpr unsigned HOST_TMP5 = 6;
		constexpr unsigned HOST_VTLB_VMAP = 7;
		constexpr unsigned HOST_VTLB_HOST_MEMORY_BASE = 8;
		constexpr unsigned HOST_GPR_PIN0 = 9;
		constexpr unsigned HOST_COP1_EXPONENT_MASK = 10;
		constexpr unsigned HOST_VU0_BASE = 11;
		constexpr u8 NO_GPR_PIN_HOST = 0xff;

		constexpr size_t GPR_OFFSET = offsetof(cpuRegisters, GPR);
		constexpr size_t HI_OFFSET = offsetof(cpuRegisters, HI);
		constexpr size_t LO_OFFSET = offsetof(cpuRegisters, LO);
		constexpr size_t CP0_OFFSET = offsetof(cpuRegisters, CP0);
		constexpr size_t FPU_OFFSET = offsetof(cpuRegistersPack, fpuRegs) - offsetof(cpuRegistersPack, cpuRegs);
		constexpr size_t FPR_OFFSET = FPU_OFFSET + offsetof(fpuRegisters, fpr);
		constexpr size_t FPRC_OFFSET = FPU_OFFSET + offsetof(fpuRegisters, fprc);
		constexpr size_t FPU_ACC_OFFSET = FPU_OFFSET + offsetof(fpuRegisters, ACC);
		constexpr size_t FPU_ACCFLAG_OFFSET = FPU_OFFSET + offsetof(fpuRegisters, ACCflag);
		constexpr size_t SA_OFFSET = offsetof(cpuRegisters, sa);
		constexpr size_t PC_OFFSET = offsetof(cpuRegisters, pc);
		constexpr size_t CODE_OFFSET = offsetof(cpuRegisters, code);
		constexpr size_t PERF_OFFSET = offsetof(cpuRegisters, PERF);
		constexpr size_t PERF_PCCR_OFFSET = PERF_OFFSET;
		constexpr size_t PERF_PCR0_OFFSET = PERF_OFFSET + sizeof(u32);
		constexpr size_t PERF_PCR1_OFFSET = PERF_OFFSET + 2 * sizeof(u32);
		constexpr size_t CYCLE_OFFSET = offsetof(cpuRegisters, cycle);
		constexpr size_t BRANCH_OFFSET = offsetof(cpuRegisters, branch);
		constexpr size_t NEXT_EVENT_OFFSET = offsetof(cpuRegisters, nextEventCycle);
		constexpr size_t LAST_COP0_CYCLE_OFFSET = offsetof(cpuRegisters, lastCOP0Cycle);
		constexpr size_t LAST_PERF_CYCLE_OFFSET = offsetof(cpuRegisters, lastPERFCycle);
		constexpr size_t TLB_ENTRY_COUNT = 48;
		constexpr size_t TLB_PAGE_MASK_OFFSET = offsetof(tlbs, PageMask);
		constexpr size_t TLB_ENTRY_HI_OFFSET = offsetof(tlbs, EntryHi);
		constexpr size_t TLB_ENTRY_LO0_OFFSET = offsetof(tlbs, EntryLo0);
		constexpr size_t TLB_ENTRY_LO1_OFFSET = offsetof(tlbs, EntryLo1);
		constexpr size_t TLB_ENTRY_SIZE = sizeof(tlbs);
		using Vu0State = std::remove_reference_t<decltype(VU0)>;
		constexpr size_t VU0_VF_OFFSET = offsetof(Vu0State, VF);
		constexpr size_t VU0_VI_OFFSET = offsetof(Vu0State, VI);
		constexpr size_t VU0_ACC_OFFSET = offsetof(Vu0State, ACC);
		constexpr size_t VU0_Q_OFFSET = offsetof(Vu0State, q);
		constexpr size_t VU0_MACFLAG_OFFSET = offsetof(Vu0State, macflag);
		constexpr size_t VU0_STATUSFLAG_OFFSET = offsetof(Vu0State, statusflag);
		constexpr size_t VU0_CLIPFLAG_OFFSET = offsetof(Vu0State, clipflag);
#if !defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		constexpr size_t VU0_MEM_OFFSET = offsetof(Vu0State, Mem);
		constexpr size_t VU0_CODE_OFFSET = offsetof(Vu0State, code);
		constexpr size_t VU0_VI_BACKUP_CYCLES_OFFSET = offsetof(Vu0State, VIBackupCycles);
		constexpr size_t VU0_VI_OLD_VALUE_OFFSET = offsetof(Vu0State, VIOldValue);
		constexpr size_t VU0_VI_REG_NUMBER_OFFSET = offsetof(Vu0State, VIRegNumber);
#endif
		constexpr size_t VU0_VF_STRIDE = sizeof(VU0.VF[0]);
		constexpr size_t VU0_VI_STRIDE = sizeof(VU0.VI[0]);
		constexpr u32 TLB_PAGE_MASK_REGISTER_MASK = 0x01ffe000u;
		constexpr u32 TLB_TLBR_ENTRY_LO0_MASK = 0x03fffffeu;
		constexpr u32 TLB_TLBR_ENTRY_LO1_MASK = 0x83fffffeu;
		constexpr u32 TLB_ENTRY_HI32_VPN2_MASK = 0x0007ffffu;
		constexpr u32 TLB_MASK_FIELD_MASK = 0x00000fffu;

		constexpr u32 GOEMON_PRELOAD_RETURN_PC_0 = 0x0033ad48;
		constexpr u32 GOEMON_PRELOAD_RETURN_PC_1 = 0x0035060c;
		constexpr u32 GOEMON_UNLOAD_ENTRY_PC = 0x003563b8;
		constexpr u32 FPU_FCR31_CONDITION_FLAG = 0x00800000;
		constexpr u32 FPU_FCR31_INVALID_FLAG = 0x00020000;
		constexpr u32 FPU_FCR31_DIVIDE_BY_ZERO_FLAG = 0x00010000;
		constexpr u32 FPU_FCR31_OVERFLOW_FLAG = 0x00008000;
		constexpr u32 FPU_FCR31_UNDERFLOW_FLAG = 0x00004000;
		constexpr u32 FPU_FCR31_STICKY_INVALID_FLAG = 0x00000040;
		constexpr u32 FPU_FCR31_STICKY_DIVIDE_BY_ZERO_FLAG = 0x00000020;
		constexpr u32 FPU_FCR31_STICKY_OVERFLOW_FLAG = 0x00000010;
		constexpr u32 FPU_FCR31_STICKY_UNDERFLOW_FLAG = 0x00000008;
		constexpr u32 FPU_FCR31_INVALID_FLAGS =
			FPU_FCR31_INVALID_FLAG | FPU_FCR31_STICKY_INVALID_FLAG;
		constexpr u32 FPU_FCR31_DIVIDE_BY_ZERO_FLAGS =
			FPU_FCR31_DIVIDE_BY_ZERO_FLAG | FPU_FCR31_STICKY_DIVIDE_BY_ZERO_FLAG;
		constexpr u32 FPU_FCR31_ARITHMETIC_OVERFLOW_FLAGS =
			FPU_FCR31_OVERFLOW_FLAG | FPU_FCR31_STICKY_OVERFLOW_FLAG;
		constexpr u32 FPU_FCR31_ARITHMETIC_UNDERFLOW_FLAGS =
			FPU_FCR31_UNDERFLOW_FLAG | FPU_FCR31_STICKY_UNDERFLOW_FLAG;
		constexpr u32 FPU_FCR31_INVALID_DIVIDE_CAUSE_FLAGS =
			FPU_FCR31_INVALID_FLAG | FPU_FCR31_DIVIDE_BY_ZERO_FLAG;
		constexpr u32 FPU_FCR31_OVERFLOW_UNDERFLOW_FLAGS = 0x0000c000;
		constexpr u32 FPU_FCR31_CLEAR_OVERFLOW_UNDERFLOW_MASK = ~FPU_FCR31_OVERFLOW_UNDERFLOW_FLAGS;
		constexpr u32 FPU_FLOAT_SIGN_MASK = 0x80000000;
		constexpr u32 FPU_FLOAT_EXPONENT_MASK = 0x7f800000;
		constexpr u32 FPU_FLOAT_IMPLICIT_MANTISSA = 0x00800000;
		constexpr u32 FPU_FLOAT_ONE = 0x3f800000;
		constexpr u32 FPU_FLOAT_MAX_FINITE = 0x7f7fffff;
		constexpr u32 FPU_CVT_W_MAX_EXPONENT_MASK = 0x4e800000;
		constexpr u32 FPU_FLOAT_EXPONENT_BIAS = 127;
		constexpr u32 FPU_FLOAT_MANTISSA_BITS = 23;
		constexpr size_t DMAC_REGS_HW_OFFSET = 0xe000;
		constexpr u16 DMAC_STAT_DMAC_OFFSET = 0x10;
		constexpr u16 DMAC_PCR_DMAC_OFFSET = 0x20;
		constexpr u32 DMAC_CPCOND_MASK = 0x3ff;
		constexpr unsigned VU0_REG_STATUS_FLAG = 16;
		constexpr unsigned VU0_REG_MAC_FLAG = 17;
		constexpr unsigned VU0_REG_CLIP_FLAG = 18;
		constexpr unsigned VU0_REG_R = 20;
		constexpr unsigned VU0_REG_I = 21;
		constexpr unsigned VU0_REG_Q = 22;
		constexpr unsigned VU0_REG_TPC = 26;
		constexpr unsigned VU0_REG_FBRST = 28;
		constexpr unsigned VU0_REG_VPU_STAT = 29;
		constexpr unsigned VU0_REG_CMSAR1 = 31;

		alignas(4) u32 s_raw_gpr0_known_zero = 1;

		struct Cop2MacroMinMaxOp
		{
			bool valid = false;
			bool take_max = false;
			bool vector_operand = false;
			bool immediate_operand = false;
			unsigned broadcast_lane = 0;
		};

		struct Cop2MacroMoveOp
		{
			bool valid = false;
			bool rotate32 = false;
		};

		enum class Cop2MacroViKind : u8
		{
			Add,
			Sub,
			AddImmediate,
			And,
			Or,
		};

		struct Cop2MacroViOp
		{
			bool valid = false;
			Cop2MacroViKind kind = Cop2MacroViKind::Add;
		};

		struct Cop2MacroViTransferOp
		{
			bool valid = false;
			bool vi_to_vf = false;
		};

		enum class Cop2MacroRandomKind : u8
		{
			WaitQ,
			RNext,
			RGet,
			RInit,
			RXor,
		};

		struct Cop2MacroRandomOp
		{
			bool valid = false;
			Cop2MacroRandomKind kind = Cop2MacroRandomKind::WaitQ;
		};

		struct Cop2MacroIndexedViMemoryOp
		{
			bool valid = false;
			bool load = false;
		};

		enum class Cop2MacroIndexedVectorMemoryKind : u8
		{
			LoadIncrement,
			StoreIncrement,
			LoadDecrement,
			StoreDecrement,
		};

		struct Cop2MacroIndexedVectorMemoryOp
		{
			bool valid = false;
			Cop2MacroIndexedVectorMemoryKind kind = Cop2MacroIndexedVectorMemoryKind::LoadIncrement;
		};

		struct Cop2MacroItofOp
		{
			bool valid = false;
			unsigned offset = 0;
		};

		struct Cop2MacroFtoiOp
		{
			bool valid = false;
			unsigned offset = 0;
		};

		enum class Cop2MacroFdivKind
		{
			Div,
			Sqrt,
			Rsqrt,
		};

		struct Cop2MacroFdivOp
		{
			bool valid = false;
			Cop2MacroFdivKind kind = Cop2MacroFdivKind::Div;
		};

		enum class Cop2MacroArithmeticKind
		{
			Add,
			Sub,
			Mul,
			MAdd,
			MSub,
			OpMula,
			OpMSub,
		};

		enum class Cop2MacroArithmeticOperand
		{
			Vector,
			BroadcastLane,
			ImmediateI,
			ImmediateQ,
		};

		struct Cop2MacroArithmeticOp
		{
			bool valid = false;
			Cop2MacroArithmeticKind kind = Cop2MacroArithmeticKind::Add;
			Cop2MacroArithmeticOperand operand = Cop2MacroArithmeticOperand::Vector;
			bool acc_destination = false;
			unsigned broadcast_lane = 0;
			bool addi_triace_hack = false;
		};

		constexpr unsigned RS(u32 op)
		{
			return (op >> 21) & 0x1f;
		}

		constexpr unsigned RT(u32 op)
		{
			return (op >> 16) & 0x1f;
		}

		constexpr unsigned RD(u32 op)
		{
			return (op >> 11) & 0x1f;
		}

		constexpr unsigned SA(u32 op)
		{
			return (op >> 6) & 0x1f;
		}

		constexpr u16 IMM_U(u32 op)
		{
			return static_cast<u16>(op);
		}

		constexpr s16 IMM_S(u32 op)
		{
			return static_cast<s16>(op);
		}

		constexpr u32 INSTRUC_TARGET(u32 op)
		{
			return op & 0x03ffffffu;
		}

		constexpr u32 BranchTarget(u32 pc, u32 op)
		{
			return pc + 4 + static_cast<s32>(IMM_S(op)) * 4;
		}

		constexpr u32 JumpTarget(u32 pc, u32 op)
		{
			return (INSTRUC_TARGET(op) << 2) | ((pc + 4) & 0xf0000000u);
		}

		constexpr Cop2MacroMinMaxOp DecodeCop2MacroMinMax(u32 op)
		{
			const unsigned function = op & 0x3f;
			if (function >= 0x10 && function <= 0x13)
				return {true, true, false, false, function - 0x10};
			if (function >= 0x14 && function <= 0x17)
				return {true, false, false, false, function - 0x14};
			if (function == 0x1d)
				return {true, true, false, true, 0};
			if (function == 0x1f)
				return {true, false, false, true, 0};
			if (function == 0x2b)
				return {true, true, true, false, 0};
			if (function == 0x2f)
				return {true, false, true, false, 0};
			return {};
		}

		constexpr Cop2MacroMoveOp DecodeCop2MacroMove(u32 op)
		{
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			if (special2_index == 0x30)
				return {true, false};
			if (special2_index == 0x31)
				return {true, true};
			return {};
		}

		constexpr Cop2MacroViOp DecodeCop2MacroVi(u32 op)
		{
			switch (op & 0x3f)
			{
				case 0x30:
					return {true, Cop2MacroViKind::Add};
				case 0x31:
					return {true, Cop2MacroViKind::Sub};
				case 0x32:
					return {true, Cop2MacroViKind::AddImmediate};
				case 0x34:
					return {true, Cop2MacroViKind::And};
				case 0x35:
					return {true, Cop2MacroViKind::Or};
				default:
					return {};
			}
		}

		constexpr Cop2MacroViTransferOp DecodeCop2MacroViTransfer(u32 op)
		{
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			if (special2_index == 0x3c)
				return {true, false};
			if (special2_index == 0x3d)
				return {true, true};
			return {};
		}

		constexpr Cop2MacroRandomOp DecodeCop2MacroRandom(u32 op)
		{
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			switch (special2_index)
			{
				case 0x3b:
					return {true, Cop2MacroRandomKind::WaitQ};
				case 0x40:
					return {true, Cop2MacroRandomKind::RNext};
				case 0x41:
					return {true, Cop2MacroRandomKind::RGet};
				case 0x42:
					return {true, Cop2MacroRandomKind::RInit};
				case 0x43:
					return {true, Cop2MacroRandomKind::RXor};
				default:
					return {};
			}
		}

		constexpr Cop2MacroIndexedViMemoryOp DecodeCop2MacroIndexedViMemory(u32 op)
		{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
			(void)op;
			return {};
#else
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			if (special2_index == 0x3e)
				return {true, true};
			if (special2_index == 0x3f)
				return {true, false};
			return {};
#endif
		}

		constexpr Cop2MacroIndexedVectorMemoryOp DecodeCop2MacroIndexedVectorMemory(u32 op)
		{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
			(void)op;
			return {};
#else
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			switch (special2_index)
			{
				case 0x34:
					return {true, Cop2MacroIndexedVectorMemoryKind::LoadIncrement};
				case 0x35:
					return {true, Cop2MacroIndexedVectorMemoryKind::StoreIncrement};
				case 0x36:
					return {true, Cop2MacroIndexedVectorMemoryKind::LoadDecrement};
				case 0x37:
					return {true, Cop2MacroIndexedVectorMemoryKind::StoreDecrement};
				default:
					return {};
			}
#endif
		}

		constexpr bool IsCop2MacroClip(u32 op)
		{
			if ((op & 0x3c) != 0x3c)
				return false;

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			return special2_index == 0x1f;
		}

		constexpr Cop2MacroItofOp DecodeCop2MacroItof(u32 op)
		{
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			switch (special2_index)
			{
				case 0x10:
					return {true, 0};
				case 0x11:
					return {true, 4};
				case 0x12:
					return {true, 12};
				case 0x13:
					return {true, 15};
				default:
					return {};
			}
		}

		constexpr Cop2MacroFtoiOp DecodeCop2MacroFtoi(u32 op)
		{
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			switch (special2_index)
			{
				case 0x14:
					return {true, 0};
				case 0x15:
					return {true, 4};
				case 0x16:
					return {true, 12};
				case 0x17:
					return {true, 15};
				default:
					return {};
			}
		}

		constexpr Cop2MacroFdivOp DecodeCop2MacroFdiv(u32 op)
		{
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			switch (special2_index)
			{
				case 0x38:
					return {true, Cop2MacroFdivKind::Div};
				case 0x39:
					return {true, Cop2MacroFdivKind::Sqrt};
				case 0x3a:
					return {true, Cop2MacroFdivKind::Rsqrt};
				default:
					return {};
			}
		}

		constexpr Cop2MacroArithmeticOp DecodeCop2MacroArithmetic(u32 op)
		{
			const u32 function = op & 0x3f;
			if (function < 0x3c)
			{
				switch (function)
	{
					case 0x00:
						case 0x01:
						case 0x02:
						case 0x03:
						return {true, Cop2MacroArithmeticKind::Add,
							Cop2MacroArithmeticOperand::BroadcastLane, false, function & 0x3};
						case 0x04:
						case 0x05:
						case 0x06:
						case 0x07:
						return {true, Cop2MacroArithmeticKind::Sub,
							Cop2MacroArithmeticOperand::BroadcastLane, false, function & 0x3};
					case 0x08:
					case 0x09:
					case 0x0a:
					case 0x0b:
						return {true, Cop2MacroArithmeticKind::MAdd,
							Cop2MacroArithmeticOperand::BroadcastLane, false, function & 0x3};
					case 0x0c:
					case 0x0d:
					case 0x0e:
					case 0x0f:
						return {true, Cop2MacroArithmeticKind::MSub,
							Cop2MacroArithmeticOperand::BroadcastLane, false, function & 0x3};
					case 0x18:
					case 0x19:
					case 0x1a:
					case 0x1b:
						return {true, Cop2MacroArithmeticKind::Mul,
							Cop2MacroArithmeticOperand::BroadcastLane, false, function & 0x3};
					case 0x1c:
						return {true, Cop2MacroArithmeticKind::Mul,
							Cop2MacroArithmeticOperand::ImmediateQ, false, 0};
					case 0x1e:
						return {true, Cop2MacroArithmeticKind::Mul,
							Cop2MacroArithmeticOperand::ImmediateI, false, 0};
					case 0x20:
						return {true, Cop2MacroArithmeticKind::Add,
							Cop2MacroArithmeticOperand::ImmediateQ, false, 0};
					case 0x21:
						return {true, Cop2MacroArithmeticKind::MAdd,
							Cop2MacroArithmeticOperand::ImmediateQ, false, 0};
					case 0x22:
						return {true, Cop2MacroArithmeticKind::Add,
							Cop2MacroArithmeticOperand::ImmediateI, false, 0, true};
					case 0x23:
						return {true, Cop2MacroArithmeticKind::MAdd,
							Cop2MacroArithmeticOperand::ImmediateI, false, 0};
					case 0x24:
						return {true, Cop2MacroArithmeticKind::Sub,
							Cop2MacroArithmeticOperand::ImmediateQ, false, 0};
					case 0x25:
						return {true, Cop2MacroArithmeticKind::MSub,
							Cop2MacroArithmeticOperand::ImmediateQ, false, 0};
					case 0x26:
						return {true, Cop2MacroArithmeticKind::Sub,
							Cop2MacroArithmeticOperand::ImmediateI, false, 0};
					case 0x27:
						return {true, Cop2MacroArithmeticKind::MSub,
							Cop2MacroArithmeticOperand::ImmediateI, false, 0};
					case 0x28:
						return {true, Cop2MacroArithmeticKind::Add,
							Cop2MacroArithmeticOperand::Vector, false, 0};
					case 0x29:
						return {true, Cop2MacroArithmeticKind::MAdd,
							Cop2MacroArithmeticOperand::Vector, false, 0};
					case 0x2a:
						return {true, Cop2MacroArithmeticKind::Mul,
							Cop2MacroArithmeticOperand::Vector, false, 0};
					case 0x2c:
						return {true, Cop2MacroArithmeticKind::Sub,
							Cop2MacroArithmeticOperand::Vector, false, 0};
					case 0x2d:
						return {true, Cop2MacroArithmeticKind::MSub,
							Cop2MacroArithmeticOperand::Vector, false, 0};
					case 0x2e:
						return {true, Cop2MacroArithmeticKind::OpMSub,
							Cop2MacroArithmeticOperand::Vector, false, 0};
					default:
						return {};
	}
			}

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			switch (special2_index)
			{
				case 0x00:
				case 0x01:
				case 0x02:
				case 0x03:
					return {true, Cop2MacroArithmeticKind::Add,
						Cop2MacroArithmeticOperand::BroadcastLane, true, special2_index & 0x3};
				case 0x04:
				case 0x05:
				case 0x06:
				case 0x07:
					return {true, Cop2MacroArithmeticKind::Sub,
						Cop2MacroArithmeticOperand::BroadcastLane, true, special2_index & 0x3};
				case 0x08:
				case 0x09:
				case 0x0a:
				case 0x0b:
					return {true, Cop2MacroArithmeticKind::MAdd,
						Cop2MacroArithmeticOperand::BroadcastLane, true, special2_index & 0x3};
				case 0x0c:
				case 0x0d:
				case 0x0e:
				case 0x0f:
					return {true, Cop2MacroArithmeticKind::MSub,
						Cop2MacroArithmeticOperand::BroadcastLane, true, special2_index & 0x3};
				case 0x18:
				case 0x19:
				case 0x1a:
				case 0x1b:
					return {true, Cop2MacroArithmeticKind::Mul,
						Cop2MacroArithmeticOperand::BroadcastLane, true, special2_index & 0x3};
				case 0x1c:
					return {true, Cop2MacroArithmeticKind::Mul,
						Cop2MacroArithmeticOperand::ImmediateQ, true, 0};
				case 0x1e:
					return {true, Cop2MacroArithmeticKind::Mul,
						Cop2MacroArithmeticOperand::ImmediateI, true, 0};
				case 0x20:
					return {true, Cop2MacroArithmeticKind::Add,
						Cop2MacroArithmeticOperand::ImmediateQ, true, 0};
				case 0x21:
					return {true, Cop2MacroArithmeticKind::MAdd,
						Cop2MacroArithmeticOperand::ImmediateQ, true, 0};
				case 0x22:
					return {true, Cop2MacroArithmeticKind::Add,
						Cop2MacroArithmeticOperand::ImmediateI, true, 0};
				case 0x23:
					return {true, Cop2MacroArithmeticKind::MAdd,
						Cop2MacroArithmeticOperand::ImmediateI, true, 0};
				case 0x24:
					return {true, Cop2MacroArithmeticKind::Sub,
						Cop2MacroArithmeticOperand::ImmediateQ, true, 0};
				case 0x25:
					return {true, Cop2MacroArithmeticKind::MSub,
						Cop2MacroArithmeticOperand::ImmediateQ, true, 0};
				case 0x26:
					return {true, Cop2MacroArithmeticKind::Sub,
						Cop2MacroArithmeticOperand::ImmediateI, true, 0};
				case 0x27:
					return {true, Cop2MacroArithmeticKind::MSub,
						Cop2MacroArithmeticOperand::ImmediateI, true, 0};
				case 0x28:
					return {true, Cop2MacroArithmeticKind::Add,
						Cop2MacroArithmeticOperand::Vector, true, 0};
				case 0x29:
					return {true, Cop2MacroArithmeticKind::MAdd,
						Cop2MacroArithmeticOperand::Vector, true, 0};
				case 0x2a:
					return {true, Cop2MacroArithmeticKind::Mul,
						Cop2MacroArithmeticOperand::Vector, true, 0};
				case 0x2c:
					return {true, Cop2MacroArithmeticKind::Sub,
						Cop2MacroArithmeticOperand::Vector, true, 0};
				case 0x2d:
					return {true, Cop2MacroArithmeticKind::MSub,
						Cop2MacroArithmeticOperand::Vector, true, 0};
				case 0x2e:
					return {true, Cop2MacroArithmeticKind::OpMula,
						Cop2MacroArithmeticOperand::Vector, true, 0};
				default:
					return {};
			}
		}

		constexpr size_t GprOffset(unsigned guest_reg)
		{
			return GPR_OFFSET + sizeof(GPR_reg) * guest_reg;
		}

		constexpr size_t Cp0Offset(unsigned guest_reg)
		{
			return CP0_OFFSET + sizeof(u32) * guest_reg;
		}

		constexpr size_t FprOffset(unsigned guest_reg)
		{
			return FPR_OFFSET + sizeof(FPRreg) * guest_reg;
		}

		constexpr size_t FprcOffset(unsigned guest_reg)
		{
			return FPRC_OFFSET + sizeof(u32) * guest_reg;
		}

		constexpr bool CanUseA32DualTransferPair(unsigned low_reg, unsigned high_reg)
		{
			return low_reg < 14 && high_reg == low_reg + 1 && ((low_reg & 1u) == 0);
		}

		u32 RotateRight32(u32 value, unsigned amount)
		{
			amount &= 31u;
			return amount == 0 ? value : ((value >> amount) | (value << (32u - amount)));
		}

		u32 RotateLeft32(u32 value, unsigned amount)
		{
			amount &= 31u;
			return amount == 0 ? value : ((value << amount) | (value >> (32u - amount)));
		}

		bool CanEncodeA32ModifiedImmediate(u32 value)
		{
			for (unsigned rotate = 0; rotate < 16; rotate++)
			{
				const unsigned amount = rotate * 2;
				const u32 imm8 = RotateLeft32(value, amount);
				if ((imm8 & ~0xffu) == 0 && RotateRight32(imm8, amount) == value)
					return true;
			}

			return false;
		}

		bool IsCheapA32AndConstantWord(u32 value)
		{
			return value == 0 || value == 0xffffffffu ||
				   CanEncodeA32ModifiedImmediate(value) || CanEncodeA32ModifiedImmediate(~value);
		}

		bool IsCheapA32LogicalConstantWord(u32 value)
		{
			return value == 0 || value == 0xffffffffu || CanEncodeA32ModifiedImmediate(value);
		}

		bool IsCheapA32AndConstant64(u32 low, u32 high)
		{
			return IsCheapA32AndConstantWord(low) && IsCheapA32AndConstantWord(high);
		}

		bool IsCheapA32LogicalConstant64(u32 low, u32 high)
		{
			return IsCheapA32LogicalConstantWord(low) && IsCheapA32LogicalConstantWord(high);
		}

		bool IsCheapA32CompareConstantWord(u32 value)
		{
			return CanEncodeA32ModifiedImmediate(value) || CanEncodeA32ModifiedImmediate(0u - value);
		}

		bool IsCheapA32CompareConstant64(u32 low, u32 high)
		{
			return IsCheapA32CompareConstantWord(low) && IsCheapA32CompareConstantWord(high);
		}

		bool IsSingleInstructionA32MoveConstant(u32 value)
		{
			return (value >> 16) == 0 || CanEncodeA32ModifiedImmediate(value) ||
				CanEncodeA32ModifiedImmediate(~value);
		}

		constexpr size_t HiloLaneOffset(size_t hilo_offset, bool upper_pipeline)
		{
			return hilo_offset + (upper_pipeline ? sizeof(u64) : 0);
		}

		constexpr size_t PackedHalfwordAccumulatorOffset(unsigned lane)
		{
			const size_t base_offset = ((lane & 2u) != 0) ? HI_OFFSET : LO_OFFSET;
			const unsigned word = (lane & 1u) + (((lane & 4u) != 0) ? 2u : 0u);
			return base_offset + word * sizeof(u32);
		}

		bool CanCompileSPECIAL(u32 op)
		{
			switch (op & 0x3f)
			{
				case 0x00: // SLL, owned by R5900OpcodeImpl.cpp::SLL().
				case 0x02: // SRL, owned by R5900OpcodeImpl.cpp::SRL().
				case 0x03: // SRA, owned by R5900OpcodeImpl.cpp::SRA().
				case 0x04: // SLLV, owned by R5900OpcodeImpl.cpp::SLLV().
				case 0x06: // SRLV, owned by R5900OpcodeImpl.cpp::SRLV().
				case 0x07: // SRAV, owned by R5900OpcodeImpl.cpp::SRAV().
				case 0x08: // JR, owned by Interpreter.cpp::JR().
				case 0x09: // JALR, owned by Interpreter.cpp::JALR().
				case 0x0a: // MOVZ, owned by R5900OpcodeImpl.cpp::MOVZ().
				case 0x0b: // MOVN, owned by R5900OpcodeImpl.cpp::MOVN().
				case 0x0c: // SYSCALL, owned by R5900OpcodeImpl.cpp::SYSCALL().
				case 0x0d: // BREAK, owned by R5900OpcodeImpl.cpp::BREAK().
				case 0x0f: // SYNC, owned by R5900OpcodeImpl.cpp::SYNC().
				case 0x10: // MFHI, owned by R5900OpcodeImpl.cpp::MFHI().
				case 0x11: // MTHI, owned by R5900OpcodeImpl.cpp::MTHI().
				case 0x12: // MFLO, owned by R5900OpcodeImpl.cpp::MFLO().
				case 0x13: // MTLO, owned by R5900OpcodeImpl.cpp::MTLO().
				case 0x14: // DSLLV, owned by R5900OpcodeImpl.cpp::DSLLV().
				case 0x16: // DSRLV, owned by R5900OpcodeImpl.cpp::DSRLV().
				case 0x17: // DSRAV, owned by R5900OpcodeImpl.cpp::DSRAV().
				case 0x18: // MULT, owned by R5900OpcodeImpl.cpp::MULT().
				case 0x19: // MULTU, owned by R5900OpcodeImpl.cpp::MULTU().
				case 0x1a: // DIV, owned by R5900OpcodeImpl.cpp::DIV().
				case 0x1b: // DIVU, owned by R5900OpcodeImpl.cpp::DIVU().
				case 0x20: // ADD, owned by R5900OpcodeImpl.cpp::ADD().
				case 0x21: // ADDU, owned by R5900OpcodeImpl.cpp::ADDU().
				case 0x22: // SUB, owned by R5900OpcodeImpl.cpp::SUB().
				case 0x23: // SUBU, owned by R5900OpcodeImpl.cpp::SUBU().
				case 0x24: // AND, owned by R5900OpcodeImpl.cpp::AND().
				case 0x25: // OR, owned by R5900OpcodeImpl.cpp::OR().
				case 0x26: // XOR, owned by R5900OpcodeImpl.cpp::XOR().
				case 0x27: // NOR, owned by R5900OpcodeImpl.cpp::NOR().
				case 0x28: // MFSA, owned by R5900OpcodeImpl.cpp::MFSA().
				case 0x29: // MTSA, owned by R5900OpcodeImpl.cpp::MTSA().
				case 0x2a: // SLT, owned by R5900OpcodeImpl.cpp::SLT().
				case 0x2b: // SLTU, owned by R5900OpcodeImpl.cpp::SLTU().
				case 0x2c: // DADD, owned by R5900OpcodeImpl.cpp::DADD().
				case 0x2d: // DADDU, owned by R5900OpcodeImpl.cpp::DADDU().
				case 0x2e: // DSUB, owned by R5900OpcodeImpl.cpp::DSUB().
				case 0x2f: // DSUBU, owned by R5900OpcodeImpl.cpp::DSUBU().
				case 0x30: // TGE, owned by R5900OpcodeImpl.cpp::TGE().
				case 0x31: // TGEU, owned by R5900OpcodeImpl.cpp::TGEU().
				case 0x32: // TLT, owned by R5900OpcodeImpl.cpp::TLT().
				case 0x33: // TLTU, owned by R5900OpcodeImpl.cpp::TLTU().
				case 0x34: // TEQ, owned by R5900OpcodeImpl.cpp::TEQ().
				case 0x36: // TNE, owned by R5900OpcodeImpl.cpp::TNE().
				case 0x38: // DSLL, owned by R5900OpcodeImpl.cpp::DSLL().
				case 0x3a: // DSRL, owned by R5900OpcodeImpl.cpp::DSRL().
				case 0x3b: // DSRA, owned by R5900OpcodeImpl.cpp::DSRA().
				case 0x3c: // DSLL32, owned by R5900OpcodeImpl.cpp::DSLL32().
				case 0x3e: // DSRL32, owned by R5900OpcodeImpl.cpp::DSRL32().
				case 0x3f: // DSRA32, owned by R5900OpcodeImpl.cpp::DSRA32().
					return true;
				default:
					return false;
			}
		}

		bool IsBREAK(u32 op)
		{
			return (op >> 26) == 0x00 && (op & 0x3f) == 0x0d;
		}

		bool IsSYSCALL(u32 op)
		{
			return (op >> 26) == 0x00 && (op & 0x3f) == 0x0c;
		}

		bool IsSYNC(u32 op)
		{
			return (op >> 26) == 0x00 && (op & 0x3f) == 0x0f;
		}

		bool IsSpecialTrap(u32 op)
		{
			if ((op >> 26) != 0x00)
				return false;

			switch (op & 0x3f)
			{
				case 0x30: // TGE
				case 0x31: // TGEU
				case 0x32: // TLT
				case 0x33: // TLTU
				case 0x34: // TEQ
				case 0x36: // TNE
					return true;
				default:
					return false;
			}
		}

		bool IsRegImmTrap(u32 op)
		{
			if ((op >> 26) != 0x01)
				return false;

			switch (RT(op))
			{
				case 0x08: // TGEI
				case 0x09: // TGEIU
				case 0x0a: // TLTI
				case 0x0b: // TLTIU
				case 0x0c: // TEQI
				case 0x0e: // TNEI
					return true;
				default:
					return false;
			}
		}

		bool IsTrapOpcode(u32 op)
		{
			return IsSpecialTrap(op) || IsRegImmTrap(op);
		}

		const void* TrapHelperForOpcode(u32 op)
		{
			using namespace R5900::Interpreter::OpcodeImpl;

			if ((op >> 26) == 0x00)
			{
				switch (op & 0x3f)
	{
					case 0x30: return reinterpret_cast<const void*>(&TGE);
					case 0x31: return reinterpret_cast<const void*>(&TGEU);
					case 0x32: return reinterpret_cast<const void*>(&TLT);
					case 0x33: return reinterpret_cast<const void*>(&TLTU);
					case 0x34: return reinterpret_cast<const void*>(&TEQ);
					case 0x36: return reinterpret_cast<const void*>(&TNE);
					default: return nullptr;
	}
			}

			if ((op >> 26) == 0x01)
			{
				switch (RT(op))
	{
					case 0x08: return reinterpret_cast<const void*>(&TGEI);
					case 0x09: return reinterpret_cast<const void*>(&TGEIU);
					case 0x0a: return reinterpret_cast<const void*>(&TLTI);
					case 0x0b: return reinterpret_cast<const void*>(&TLTIU);
					case 0x0c: return reinterpret_cast<const void*>(&TEQI);
					case 0x0e: return reinterpret_cast<const void*>(&TNEI);
					default: return nullptr;
	}
			}

			return nullptr;
		}

		bool IsCounterReadLoad(u32 op)
		{
			switch (op >> 26)
			{
				case 0x20: // LB, owned by R5900OpcodeImpl.cpp::LB().
				case 0x21: // LH, owned by R5900OpcodeImpl.cpp::LH().
				case 0x23: // LW, owned by R5900OpcodeImpl.cpp::LW().
				case 0x24: // LBU, owned by R5900OpcodeImpl.cpp::LBU().
				case 0x25: // LHU, owned by R5900OpcodeImpl.cpp::LHU().
					return true;
				default:
					return false;
			}
		}

		bool CanCompileMMI0(u32 op);
		bool CanCompileMMI1(u32 op);
		bool CanCompileMMI2(u32 op);
		bool CanCompileMMI3(u32 op);
		bool CanCompileCOP0(u32 op);
		bool CanCompileCOP1(u32 op);
		bool CanCompileCOP2(u32 op);

		bool IsFastMFC0(u32 op)
		{
			if ((op >> 26) != 0x10 || ((op >> 21) & 0x1f) != 0x00)
				return false;

			const unsigned rd = RD(op);
			// PCSX2 x86/iCOP0.cpp::recMFC0() keeps Count in-block by committing
			// cycles through scaleblockcycles_clear(). MFPS/PCCR stays in-block,
			// and PCR0/PCR1 reads call COP0_UpdatePCCR() before reading the
			// selected counter without forcing an event-tail split.
			if (rd != 25)
				return true;

			return true;
		}

		bool IsFastMTC0(u32 op)
		{
			if ((op >> 26) != 0x10 || ((op >> 21) & 0x1f) != 0x04)
				return false;

			switch (RD(op))
			{
				case 0x09: // Count, owned by x86/iCOP0.cpp::recMTC0().
				case 0x10: // Config, owned by COP0.cpp::WriteCP0Config().
				case 0x18: // Breakpoint debug registers only log in PCSX2.
					return true;
				case 0x0c: // Status, owned by x86/iCOP0.cpp::recMTC0().
					return true;
				case 0x19: // Perf counters.
					return true;
				default:
					return true;
			}
		}

		bool IsDI(u32 op)
		{
			return (op >> 26) == 0x10 && ((op >> 21) & 0x1f) == 0x10 && (op & 0x3f) == 0x39;
		}

		bool IsInBlockTLBReadProbe(u32 op)
		{
			if ((op >> 26) != 0x10 || ((op >> 21) & 0x1f) != 0x10)
				return false;

			switch (op & 0x3f)
			{
				case 0x01: // TLBR, owned by COP0.cpp::TLBR().
				case 0x08: // TLBP, owned by COP0.cpp::TLBP().
					return true;
				default:
					return false;
			}
		}

		bool IsCycleCommittingFastCOP0(u32 op)
		{
			if ((op >> 26) != 0x10)
				return false;

			switch ((op >> 21) & 0x1f)
			{
				case 0x00:
					return RD(op) == 9 || (RD(op) == 25 && RT(op) != 0 && (op & 1u) != 0);
				case 0x04:
					return RD(op) == 9 || RD(op) == 12 ||
						   (RD(op) == 25 && ((op & 1u) != 0 || (op & 0x3fu) == 0));
				default:
					return false;
			}
		}

		bool IsFastCOP1MoveControl(u32 op)
		{
			if ((op >> 26) != 0x11)
				return false;

			switch ((op >> 21) & 0x1f)
			{
				case 0x00: // MFC1
				case 0x02: // CFC1
				case 0x04: // MTC1
				case 0x06: // CTC1
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP1ScalarWordOp(u32 op)
		{
			if ((op >> 26) != 0x11 || ((op >> 21) & 0x1f) != 0x10)
				return false;

			switch (op & 0x3f)
			{
				case 0x05: // ABS_S, owned by FPU.cpp::ABS_S().
				case 0x06: // MOV_S, owned by FPU.cpp::MOV_S().
				case 0x07: // NEG_S, owned by FPU.cpp::NEG_S().
				case 0x28: // MAX_S, owned by FPU.cpp::MAX_S().
				case 0x29: // MIN_S, owned by FPU.cpp::MIN_S().
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP1ArithmeticOp(u32 op)
		{
			if ((op >> 26) != 0x11 || ((op >> 21) & 0x1f) != 0x10)
				return false;

			switch (op & 0x3f)
			{
				case 0x00: // ADD_S, owned by FPU.cpp::ADD_S().
				case 0x01: // SUB_S, owned by FPU.cpp::SUB_S().
				case 0x02: // MUL_S, owned by FPU.cpp::MUL_S().
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP1DivSqrtOp(u32 op)
		{
			if ((op >> 26) != 0x11 || ((op >> 21) & 0x1f) != 0x10)
				return false;

			switch (op & 0x3f)
			{
				case 0x03: // DIV_S, owned by FPU.cpp::DIV_S().
				case 0x04: // SQRT_S, owned by FPU.cpp::SQRT_S().
				case 0x16: // RSQRT_S, owned by FPU.cpp::RSQRT_S().
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP1AccumulatorOp(u32 op)
		{
			if ((op >> 26) != 0x11 || ((op >> 21) & 0x1f) != 0x10)
				return false;

			switch (op & 0x3f)
			{
				case 0x18: // ADDA_S, owned by FPU.cpp::ADDA_S().
				case 0x19: // SUBA_S, owned by FPU.cpp::SUBA_S().
				case 0x1a: // MULA_S, owned by FPU.cpp::MULA_S().
				case 0x1c: // MADD_S, owned by FPU.cpp::MADD_S().
				case 0x1d: // MSUB_S, owned by FPU.cpp::MSUB_S().
				case 0x1e: // MADDA_S, owned by FPU.cpp::MADDA_S().
				case 0x1f: // MSUBA_S, owned by FPU.cpp::MSUBA_S().
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP1CompareOp(u32 op)
		{
			if ((op >> 26) != 0x11 || ((op >> 21) & 0x1f) != 0x10)
				return false;

			switch (op & 0x3f)
			{
				case 0x30: // C_F, owned by FPU.cpp::C_F().
				case 0x32: // C_EQ, owned by FPU.cpp::C_EQ().
				case 0x34: // C_LT, owned by FPU.cpp::C_LT().
				case 0x36: // C_LE, owned by FPU.cpp::C_LE().
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP1ConvertWordOp(u32 op)
		{
			return (op >> 26) == 0x11 && ((op >> 21) & 0x1f) == 0x10 &&
				   (op & 0x3f) == 0x24; // CVT_W, owned by FPU.cpp::CVT_W().
		}

		bool IsFastCOP1ConvertSingleOp(u32 op)
		{
			return (op >> 26) == 0x11 && ((op >> 21) & 0x1f) == 0x14 &&
				   (op & 0x3f) == 0x20; // CVT_S, owned by FPU.cpp::CVT_S().
		}

		bool IsFastCOP1InBlock(u32 op)
		{
			return IsFastCOP1MoveControl(op) || IsFastCOP1ArithmeticOp(op) ||
				   IsFastCOP1DivSqrtOp(op) ||
				   IsFastCOP1AccumulatorOp(op) ||
				   IsFastCOP1ScalarWordOp(op) ||
				   IsFastCOP1CompareOp(op) || IsFastCOP1ConvertWordOp(op) ||
				   IsFastCOP1ConvertSingleOp(op);
		}

		bool FastCOP1UsesExponentMask(u32 op)
		{
			if (IsFastCOP1ArithmeticOp(op) || IsFastCOP1DivSqrtOp(op) ||
				IsFastCOP1AccumulatorOp(op) || IsFastCOP1ConvertWordOp(op))
			{
				return true;
			}

			return IsFastCOP1CompareOp(op) && (op & 0x3f) != 0x30; // C_F only clears FCR31.C.
		}

		bool IsCOP2Special2Supported(u32 index)
		{
			// PCSX2 owner: R5900OpcodeTables.cpp::Int_COP2SPECIAL2PrintTable.
			// Keep unknown slots out of the A32 provider so they remain visible
			// as scan fallbacks instead of helper-calling COP2_Unknown().
			return (index <= 42) || (index >= 44 && index <= 49) ||
				   (index >= 52 && index <= 67);
		}

		bool IsCOP2Special1Supported(u32 op)
		{
			// PCSX2 owner: R5900OpcodeTables.cpp::Int_COP2SPECIAL1PrintTable.
			const u32 function = op & 0x3f;
			if (function <= 50 || function == 52 || function == 53 ||
				function == 56 || function == 57)
			{
				return true;
			}

			if (function >= 60)
			{
				const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
				return IsCOP2Special2Supported(special2_index);
			}

			return false;
		}

		bool IsCOP2BranchOpcode(u32 op)
		{
			if ((op >> 26) != 0x12 || ((op >> 21) & 0x1f) != 0x08)
				return false;

			switch (RT(op))
			{
				case 0x00: // BC2F, owned by COP2.cpp::BC2F().
				case 0x01: // BC2T, owned by COP2.cpp::BC2T().
				case 0x02: // BC2FL, owned by COP2.cpp::BC2FL().
				case 0x03: // BC2TL, owned by COP2.cpp::BC2TL().
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP2VectorTransfer(u32 op)
		{
			if ((op >> 26) != 0x12)
				return false;

			switch ((op >> 21) & 0x1f)
			{
				case 0x01: // QMFC2, owned by VU0.cpp::QMFC2().
				case 0x05: // QMTC2, owned by VU0.cpp::QMTC2().
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP2ControlRead(u32 op)
		{
			return (op >> 26) == 0x12 && ((op >> 21) & 0x1f) == 0x02; // CFC2, owned by VU0.cpp::CFC2().
		}

		bool IsFastCOP2ControlWrite(u32 op)
		{
			if ((op >> 26) != 0x12 || ((op >> 21) & 0x1f) != 0x06)
			{
				return false;
			}

			return true;
		}

		bool IsFastCOP2MacroInBlock(u32 op)
		{
			// PCSX2 owners: VU0.cpp::COP2_SPECIAL(), VUops.cpp VADD/VSUB/
			// VMUL/VMADD/VMSUB/VOPMULA/VOPMSUB/VMAX/VMINI/VABS/VCLIP/VDIV/VFTOI/VITOF/VNOP/VMOVE/VMR32/VI*/VMFIR/VMTIR/VWAITQ/VR*/VILWR/VISWR/
			// VLQI/VSQI/VLQD/VSQD, and x86/microVU_Macro.inl
			// recVADD/recVSUB/recVMUL/recVMADD/recVMSUB/recVOPMULA/recVOPMSUB/recVMAX/recVMINI/recVABS/recVNOP/
			// recVMOVE/recVMR32/recVI*/recVMFIR/recVMTIR/recVWAITQ/recVR*. These macro ops either have
			// no MAC/status/clip synchronization side effects or synchronize
			// their flags directly, so idle VU0 can execute them inline while
			// running VU0 stays on the helper tail.
			if ((op >> 26) != 0x12 || (((op >> 21) & 0x10) == 0) ||
				!IsCOP2Special1Supported(op))
			{
				return false;
			}

			if (DecodeCop2MacroArithmetic(op).valid)
				return true;
			if (DecodeCop2MacroMinMax(op).valid)
				return true;
			if (DecodeCop2MacroMove(op).valid)
				return true;
			if (DecodeCop2MacroVi(op).valid)
				return true;
			if (DecodeCop2MacroViTransfer(op).valid)
				return true;
			if (DecodeCop2MacroRandom(op).valid)
				return true;
			if (DecodeCop2MacroIndexedViMemory(op).valid)
				return true;
			if (DecodeCop2MacroIndexedVectorMemory(op).valid)
				return true;
			if (IsCop2MacroClip(op))
				return true;
			if (DecodeCop2MacroItof(op).valid)
				return true;
			if (DecodeCop2MacroFtoi(op).valid)
				return true;
			if (DecodeCop2MacroFdiv(op).valid)
				return true;

			if ((op & 0x3c) != 0x3c)
				return false;

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			return special2_index == 0x1d || // VABS
				   special2_index == 0x2f;   // VNOP
		}

		bool IsFastCOP2InBlock(u32 op)
		{
			return IsFastCOP2VectorTransfer(op) || IsFastCOP2ControlRead(op) ||
				   (IsFastCOP2ControlWrite(op) && RD(op) != VU0_REG_CMSAR1) ||
				   IsFastCOP2MacroInBlock(op);
		}

		unsigned FastVu0AddressUses(u32 op)
		{
			if (IsCOP2BranchOpcode(op))
				return 1;

			if (IsFastCOP2VectorTransfer(op))
			{
				switch ((op >> 21) & 0x1f)
	{
					case 0x01: // QMFC2 checks VU0 run state and reads VF when rt is writable.
						return RT(op) != 0 ? 1 : 0;
					case 0x05: // QMTC2 writes VF when fs is writable.
						return RD(op) != 0 ? 1 : 0;
					default:
						return 0;
	}
			}

			if (IsFastCOP2ControlRead(op))
				return RT(op) != 0 ? 1 : 0;

			if (IsFastCOP2ControlWrite(op))
			{
				switch (RD(op))
	{
					case 0:
					case VU0_REG_MAC_FLAG:
					case VU0_REG_TPC:
					case VU0_REG_VPU_STAT:
						return 0;
					default:
						return 1;
	}
			}

			if (IsFastCOP2MacroInBlock(op))
			{
				if (DecodeCop2MacroArithmetic(op).valid)
					return 5; // code + source/operand/destination + MAC/status mirrors.
				if (DecodeCop2MacroMinMax(op).valid)
					return 4; // code + source VF + operand VF/VI + destination VF.
				if (DecodeCop2MacroMove(op).valid)
					return 3; // code + source VF + destination VF.
				if (DecodeCop2MacroVi(op).valid)
					return 4; // code + source VI + operand/imm + destination VI.
				if (DecodeCop2MacroViTransfer(op).valid)
					return 3; // code + source VF/VI + destination VF/VI.
				if (DecodeCop2MacroRandom(op).valid)
	{
					const Cop2MacroRandomOp random = DecodeCop2MacroRandom(op);
					return random.kind == Cop2MacroRandomKind::WaitQ ? 1 : 3;
	}
				if (DecodeCop2MacroIndexedViMemory(op).valid)
					return 4; // code + address VI + data VI + VU memory/register window.
				if (DecodeCop2MacroIndexedVectorMemory(op).valid)
					return 5; // code + VI backup/update + VF + VU memory/register window.
				if (IsCop2MacroClip(op))
					return 4; // code + Fs/Ft VF + clipflag VI mirror.
				if (DecodeCop2MacroItof(op).valid)
					return 3; // code + source VF + destination VF.
				if (DecodeCop2MacroFtoi(op).valid)
					return 3; // code + source VF + destination VF.
				if (DecodeCop2MacroFdiv(op).valid)
					return 5; // code + source VF(s) + Q/status mirrors.

				const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
				return special2_index == 0x1d ? 3 : 1; // VABS uses code + source/dest VF; VNOP uses code only.
			}

			switch (op >> 26)
			{
				case 0x36: // LQC2 checks VU0 run state and may write VF.
					return RT(op) != 0 ? 2 : 1;
				case 0x3e: // SQC2 checks VU0 run state and reads VF unless storing constant VF0.
					return RT(op) != 0 ? 2 : 1;
				default:
					return 0;
			}
		}

		bool CanCompileMMI(u32 op)
		{
			switch (op & 0x3f)
			{
				case 0x00: // MADD, owned by MMI.cpp::MADD() and x86/ix86-32/iR5900MultDiv.cpp::recMADD().
				case 0x01: // MADDU, owned by MMI.cpp::MADDU() and x86/ix86-32/iR5900MultDiv.cpp::recMADDU().
				case 0x04: // PLZCW, owned by MMI.cpp::PLZCW().
				case 0x10: // MFHI1, owned by MMI.cpp::MFHI1().
				case 0x11: // MTHI1, owned by MMI.cpp::MTHI1().
				case 0x12: // MFLO1, owned by MMI.cpp::MFLO1().
				case 0x13: // MTLO1, owned by MMI.cpp::MTLO1().
				case 0x18: // MULT1, owned by MMI.cpp::MULT1() and x86/ix86-32/iR5900MultDiv.cpp::recMULT1().
				case 0x19: // MULTU1, owned by MMI.cpp::MULTU1() and x86/ix86-32/iR5900MultDiv.cpp::recMULTU1().
				case 0x1a: // DIV1, owned by MMI.cpp::DIV1() and x86/ix86-32/iR5900MultDiv.cpp::recDIV1().
				case 0x1b: // DIVU1, owned by MMI.cpp::DIVU1() and x86/ix86-32/iR5900MultDiv.cpp::recDIVU1().
				case 0x20: // MADD1, owned by MMI.cpp::MADD1() and x86/ix86-32/iR5900MultDiv.cpp::recMADD1().
				case 0x21: // MADDU1, owned by MMI.cpp::MADDU1() and x86/ix86-32/iR5900MultDiv.cpp::recMADDU1().
				case 0x30: // PMFHL, owned by MMI.cpp::PMFHL().
				case 0x31: // PMTHL, owned by MMI.cpp::PMTHL().
				case 0x34: // PSLLH, owned by MMI.cpp::PSLLH().
				case 0x36: // PSRLH, owned by MMI.cpp::PSRLH().
				case 0x37: // PSRAH, owned by MMI.cpp::PSRAH().
				case 0x3c: // PSLLW, owned by MMI.cpp::PSLLW().
				case 0x3e: // PSRLW, owned by MMI.cpp::PSRLW().
				case 0x3f: // PSRAW, owned by MMI.cpp::PSRAW().
					return true;
				case 0x08: // MMI0 class, owned by R5900OpcodeTables.cpp::Class_MMI0().
					return CanCompileMMI0(op);
				case 0x09: // MMI2 class, owned by R5900OpcodeTables.cpp::Class_MMI2().
					return CanCompileMMI2(op);
				case 0x28: // MMI1 class, owned by R5900OpcodeTables.cpp::Class_MMI1().
					return CanCompileMMI1(op);
				case 0x29: // MMI3 class, owned by R5900OpcodeTables.cpp::Class_MMI3().
					return CanCompileMMI3(op);
				default:
					return false;
			}
		}

		bool CanCompileMMI0(u32 op)
		{
			switch ((op >> 6) & 0x1f)
			{
				case 0x00: // PADDW, owned by MMI.cpp::PADDW().
				case 0x01: // PSUBW, owned by MMI.cpp::PSUBW().
				case 0x02: // PCGTW, owned by MMI.cpp::PCGTW().
				case 0x03: // PMAXW, owned by MMI.cpp::PMAXW().
				case 0x04: // PADDH, owned by MMI.cpp::PADDH().
				case 0x05: // PSUBH, owned by MMI.cpp::PSUBH().
				case 0x06: // PCGTH, owned by MMI.cpp::PCGTH().
				case 0x07: // PMAXH, owned by MMI.cpp::PMAXH().
				case 0x08: // PADDB, owned by MMI.cpp::PADDB().
				case 0x09: // PSUBB, owned by MMI.cpp::PSUBB().
				case 0x0a: // PCGTB, owned by MMI.cpp::PCGTB().
				case 0x10: // PADDSW, owned by MMI.cpp::PADDSW().
				case 0x11: // PSUBSW, owned by MMI.cpp::PSUBSW().
				case 0x12: // PEXTLW, owned by MMI.cpp::PEXTLW().
				case 0x13: // PPACW, owned by MMI.cpp::PPACW().
				case 0x14: // PADDSH, owned by MMI.cpp::PADDSH().
				case 0x15: // PSUBSH, owned by MMI.cpp::PSUBSH().
				case 0x16: // PEXTLH, owned by MMI.cpp::PEXTLH().
				case 0x17: // PPACH, owned by MMI.cpp::PPACH().
				case 0x18: // PADDSB, owned by MMI.cpp::PADDSB().
				case 0x19: // PSUBSB, owned by MMI.cpp::PSUBSB().
				case 0x1a: // PEXTLB, owned by MMI.cpp::PEXTLB().
				case 0x1b: // PPACB, owned by MMI.cpp::PPACB().
				case 0x1e: // PEXT5, owned by MMI.cpp::PEXT5().
				case 0x1f: // PPAC5, owned by MMI.cpp::PPAC5().
					return true;
				default:
					return false;
			}
		}

		bool CanCompileMMI1(u32 op)
		{
			switch ((op >> 6) & 0x1f)
			{
				case 0x01: // PABSW, owned by MMI.cpp::PABSW().
				case 0x02: // PCEQW, owned by MMI.cpp::PCEQW().
				case 0x03: // PMINW, owned by MMI.cpp::PMINW().
				case 0x04: // PADSBH, owned by MMI.cpp::PADSBH().
				case 0x05: // PABSH, owned by MMI.cpp::PABSH().
				case 0x06: // PCEQH, owned by MMI.cpp::PCEQH().
				case 0x07: // PMINH, owned by MMI.cpp::PMINH().
				case 0x0a: // PCEQB, owned by MMI.cpp::PCEQB().
				case 0x10: // PADDUW, owned by MMI.cpp::PADDUW().
				case 0x11: // PSUBUW, owned by MMI.cpp::PSUBUW().
				case 0x12: // PEXTUW, owned by MMI.cpp::PEXTUW().
				case 0x14: // PADDUH, owned by MMI.cpp::PADDUH().
				case 0x15: // PSUBUH, owned by MMI.cpp::PSUBUH().
				case 0x16: // PEXTUH, owned by MMI.cpp::PEXTUH().
				case 0x18: // PADDUB, owned by MMI.cpp::PADDUB().
				case 0x19: // PSUBUB, owned by MMI.cpp::PSUBUB().
				case 0x1a: // PEXTUB, owned by MMI.cpp::PEXTUB().
				case 0x1b: // QFSRV, owned by MMI.cpp::QFSRV().
					return true;
				default:
					return false;
			}
		}

		bool CanCompileMMI2(u32 op)
		{
			switch ((op >> 6) & 0x1f)
			{
				case 0x00: // PMADDW, owned by MMI.cpp::PMADDW().
				case 0x02: // PSLLVW, owned by MMI.cpp::PSLLVW().
				case 0x03: // PSRLVW, owned by MMI.cpp::PSRLVW().
				case 0x04: // PMSUBW, owned by MMI.cpp::PMSUBW().
				case 0x08: // PMFHI, owned by MMI.cpp::PMFHI().
				case 0x09: // PMFLO, owned by MMI.cpp::PMFLO().
				case 0x0a: // PINTH, owned by MMI.cpp::PINTH().
				case 0x0c: // PMULTW, owned by MMI.cpp::PMULTW().
				case 0x0d: // PDIVW, owned by MMI.cpp::PDIVW().
				case 0x0e: // PCPYLD, owned by MMI.cpp::PCPYLD().
				case 0x10: // PMADDH, owned by MMI.cpp::PMADDH().
				case 0x11: // PHMADH, owned by MMI.cpp::PHMADH().
				case 0x12: // PAND, owned by MMI.cpp::PAND().
				case 0x13: // PXOR, owned by MMI.cpp::PXOR().
				case 0x14: // PMSUBH, owned by MMI.cpp::PMSUBH().
				case 0x15: // PHMSBH, owned by MMI.cpp::PHMSBH().
				case 0x1a: // PEXEH, owned by MMI.cpp::PEXEH().
				case 0x1b: // PREVH, owned by MMI.cpp::PREVH().
				case 0x1c: // PMULTH, owned by MMI.cpp::PMULTH().
				case 0x1d: // PDIVBW, owned by MMI.cpp::PDIVBW().
				case 0x1e: // PEXEW, owned by MMI.cpp::PEXEW().
				case 0x1f: // PROT3W, owned by MMI.cpp::PROT3W().
					return true;
				default:
					return false;
			}
		}

		bool CanCompileMMI3(u32 op)
		{
			switch ((op >> 6) & 0x1f)
			{
				case 0x00: // PMADDUW, owned by MMI.cpp::PMADDUW().
				case 0x03: // PSRAVW, owned by MMI.cpp::PSRAVW().
				case 0x08: // PMTHI, owned by MMI.cpp::PMTHI().
				case 0x09: // PMTLO, owned by MMI.cpp::PMTLO().
				case 0x0a: // PINTEH, owned by MMI.cpp::PINTEH().
				case 0x0c: // PMULTUW, owned by MMI.cpp::PMULTUW().
				case 0x0d: // PDIVUW, owned by MMI.cpp::PDIVUW().
				case 0x0e: // PCPYUD, owned by MMI.cpp::PCPYUD().
				case 0x12: // POR, owned by MMI.cpp::POR().
				case 0x13: // PNOR, owned by MMI.cpp::PNOR().
				case 0x1a: // PEXCH, owned by MMI.cpp::PEXCH().
				case 0x1b: // PCPYH, owned by MMI.cpp::PCPYH().
				case 0x1e: // PEXCW, owned by MMI.cpp::PEXCW().
					return true;
				default:
					return false;
			}
		}

		bool CanCompileCOP0(u32 op)
		{
			switch ((op >> 21) & 0x1f)
			{
				case 0x00: // MFC0, owned by COP0.cpp::MFC0().
				case 0x04: // MTC0, owned by COP0.cpp::MTC0().
					return true;
				case 0x08: // COP0_BC0 branch forms, owned by COP0.cpp::BC0*().
					switch (RT(op))
	{
						case 0x00: // BC0F, owned by COP0.cpp::BC0F().
						case 0x01: // BC0T, owned by COP0.cpp::BC0T().
						case 0x02: // BC0FL, owned by COP0.cpp::BC0FL().
						case 0x03: // BC0TL, owned by COP0.cpp::BC0TL().
							return true;
						default:
							return false;
	}
				case 0x10: // COP0_C0 class, owned by R5900OpcodeTables.cpp::tbl_COP0_C0.
					switch (op & 0x3f)
	{
						case 0x01: // TLBR, owned by COP0.cpp::TLBR().
						case 0x02: // TLBWI, owned by COP0.cpp::TLBWI().
						case 0x06: // TLBWR, owned by COP0.cpp::TLBWR().
						case 0x08: // TLBP, owned by COP0.cpp::TLBP().
						case 0x18: // ERET, owned by COP0.cpp::ERET().
						case 0x38: // EI, owned by COP0.cpp::EI().
						case 0x39: // DI, owned by x86/iCOP0.cpp::recDI().
							return true;
						default:
							return false;
	}
				default:
					return false;
			}
		}

		bool CanCompileCOP1(u32 op)
		{
			switch ((op >> 21) & 0x1f)
			{
				case 0x00: // MFC1, owned by FPU.cpp::MFC1().
				case 0x02: // CFC1, owned by FPU.cpp::CFC1().
				case 0x04: // MTC1, owned by FPU.cpp::MTC1().
				case 0x06: // CTC1, owned by FPU.cpp::CTC1().
					return true;
				case 0x08: // COP1_BC1 class, owned by R5900OpcodeTables.cpp::tbl_COP1_BC1.
					switch (RT(op))
	{
						case 0x00: // BC1F, owned by FPU.cpp::BC1F().
						case 0x01: // BC1T, owned by FPU.cpp::BC1T().
						case 0x02: // BC1FL, owned by FPU.cpp::BC1FL().
						case 0x03: // BC1TL, owned by FPU.cpp::BC1TL().
							return true;
						default:
							return false;
	}
				case 0x10: // COP1_S class, owned by R5900OpcodeTables.cpp::tbl_COP1_S.
					switch (op & 0x3f)
	{
						case 0x00: // ADD_S, owned by FPU.cpp::ADD_S().
						case 0x01: // SUB_S, owned by FPU.cpp::SUB_S().
						case 0x02: // MUL_S, owned by FPU.cpp::MUL_S().
						case 0x03: // DIV_S, owned by FPU.cpp::DIV_S().
						case 0x04: // SQRT_S, owned by FPU.cpp::SQRT_S().
						case 0x05: // ABS_S, owned by FPU.cpp::ABS_S().
						case 0x06: // MOV_S, owned by FPU.cpp::MOV_S().
						case 0x07: // NEG_S, owned by FPU.cpp::NEG_S().
						case 0x16: // RSQRT_S, owned by FPU.cpp::RSQRT_S().
						case 0x18: // ADDA_S, owned by FPU.cpp::ADDA_S().
						case 0x19: // SUBA_S, owned by FPU.cpp::SUBA_S().
						case 0x1a: // MULA_S, owned by FPU.cpp::MULA_S().
						case 0x1c: // MADD_S, owned by FPU.cpp::MADD_S().
						case 0x1d: // MSUB_S, owned by FPU.cpp::MSUB_S().
						case 0x1e: // MADDA_S, owned by FPU.cpp::MADDA_S().
						case 0x1f: // MSUBA_S, owned by FPU.cpp::MSUBA_S().
						case 0x24: // CVT_W, owned by FPU.cpp::CVT_W().
						case 0x28: // MAX_S, owned by FPU.cpp::MAX_S().
						case 0x29: // MIN_S, owned by FPU.cpp::MIN_S().
						case 0x30: // C_F, owned by FPU.cpp::C_F().
						case 0x32: // C_EQ, owned by FPU.cpp::C_EQ().
						case 0x34: // C_LT, owned by FPU.cpp::C_LT().
						case 0x36: // C_LE, owned by FPU.cpp::C_LE().
							return true;
						default:
							return false;
	}
				case 0x14: // COP1_W class, owned by R5900OpcodeTables.cpp::tbl_COP1_W.
					return (op & 0x3f) == 0x20; // CVT_S, owned by FPU.cpp::CVT_S().
				default:
					return false;
			}
		}

		bool CanCompileCOP2(u32 op)
		{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
			return IsCOP2BranchOpcode(op) || IsFastCOP2VectorTransfer(op) ||
				   IsFastCOP2ControlRead(op) || IsFastCOP2ControlWrite(op);
#else
			// PCSX2 owners: COP2.cpp, VU0.cpp, VUops.cpp, and
			// R5900OpcodeTables.cpp::Int_COP2*PrintTable. The first A32 full-core
			// path helper-calls the same interpreter owner for valid transfer and
			// VU0 macro forms, while BC2 branches use a native branch test below.
			switch ((op >> 21) & 0x1f)
			{
				case 0x01: // QMFC2, owned by VU0.cpp::QMFC2().
				case 0x02: // CFC2, owned by VU0.cpp::CFC2().
				case 0x05: // QMTC2, owned by VU0.cpp::QMTC2().
				case 0x06: // CTC2, owned by VU0.cpp::CTC2().
					return true;
				case 0x08: // COP2_BC2 branch forms, owned by COP2.cpp::BC2*().
					return IsCOP2BranchOpcode(op);
				default:
					if (((op >> 21) & 0x10) == 0)
						return false;
					return IsCOP2Special1Supported(op);
			}
#endif
		}

		bool IsNoOpCACHE(u32 op)
		{
			switch (RT(op))
			{
				case 0x07: // IXIN, owned by Cache.cpp::CACHE(); PCSX2 no-ops instruction-cache invalidation.
				case 0x0c: // BFH, owned by Cache.cpp::CACHE(); PCSX2 no-ops BTAC flush.
					return true;
				default:
					return false;
			}
		}

		bool IsHelperCACHE(u32 op)
		{
			switch (RT(op))
			{
				case 0x10: // DXLTG, owned by Cache.cpp::CACHE().
				case 0x11: // DXLDT, owned by Cache.cpp::CACHE().
				case 0x12: // DXSTG, owned by Cache.cpp::CACHE().
				case 0x13: // DXSDT, owned by Cache.cpp::CACHE().
				case 0x14: // DXWBIN, owned by Cache.cpp::CACHE().
				case 0x16: // DXIN, owned by Cache.cpp::CACHE().
				case 0x18: // DHWBIN, owned by Cache.cpp::CACHE().
				case 0x1a: // DHIN, owned by Cache.cpp::CACHE().
				case 0x1c: // DHWOIN, owned by Cache.cpp::CACHE().
					return true;
				default:
					return false;
			}
		}

		bool CanCompileCACHE(u32 op)
		{
			return IsNoOpCACHE(op) || IsHelperCACHE(op);
		}

		bool CanCompileREGIMM(u32 op)
		{
			switch (RT(op))
			{
				case 0x00: // BLTZ, owned by Interpreter.cpp::BLTZ().
				case 0x01: // BGEZ, owned by Interpreter.cpp::BGEZ().
				case 0x02: // BLTZL, owned by Interpreter.cpp::BLTZL().
				case 0x03: // BGEZL, owned by Interpreter.cpp::BGEZL().
				case 0x08: // TGEI, owned by R5900OpcodeImpl.cpp::TGEI().
				case 0x09: // TGEIU, owned by R5900OpcodeImpl.cpp::TGEIU().
				case 0x0a: // TLTI, owned by R5900OpcodeImpl.cpp::TLTI().
				case 0x0b: // TLTIU, owned by R5900OpcodeImpl.cpp::TLTIU().
				case 0x0c: // TEQI, owned by R5900OpcodeImpl.cpp::TEQI().
				case 0x0e: // TNEI, owned by R5900OpcodeImpl.cpp::TNEI().
				case 0x10: // BLTZAL, owned by Interpreter.cpp::BLTZAL().
				case 0x11: // BGEZAL, owned by Interpreter.cpp::BGEZAL().
				case 0x12: // BLTZALL, owned by Interpreter.cpp::BLTZALL().
				case 0x13: // BGEZALL, owned by Interpreter.cpp::BGEZALL().
				case 0x18: // MTSAB, owned by R5900OpcodeImpl.cpp::MTSAB().
				case 0x19: // MTSAH, owned by R5900OpcodeImpl.cpp::MTSAH().
					return true;
				default:
					return false;
			}
		}

		bool IsBranchLikelyOpcode(u32 op)
		{
			switch (op >> 26)
			{
				case 0x01:
					switch (RT(op))
	{
						case 0x02: // BLTZL, owned by Interpreter.cpp::BLTZL().
						case 0x03: // BGEZL, owned by Interpreter.cpp::BGEZL().
						case 0x12: // BLTZALL, owned by Interpreter.cpp::BLTZALL().
						case 0x13: // BGEZALL, owned by Interpreter.cpp::BGEZALL().
							return true;
						default:
							return false;
	}
				case 0x14: // BEQL, owned by Interpreter.cpp::BEQL().
				case 0x15: // BNEL, owned by Interpreter.cpp::BNEL().
				case 0x16: // BLEZL, owned by Interpreter.cpp::BLEZL().
				case 0x17: // BGTZL, owned by Interpreter.cpp::BGTZL().
					return true;
				case 0x11:
					return ((op >> 21) & 0x1f) == 0x08 && (RT(op) == 0x02 || RT(op) == 0x03);
				case 0x12:
					return ((op >> 21) & 0x1f) == 0x08 && (RT(op) == 0x02 || RT(op) == 0x03);
				case 0x10:
					return ((op >> 21) & 0x1f) == 0x08 && (RT(op) == 0x02 || RT(op) == 0x03);
				default:
					return false;
			}
		}

		u32 ScaleBlockCycles(u32 raw_cycles)
		{
			// Ported from PCSX2 x86/ix86-32/iR5900.cpp::scaleblockcycles_calculation()
			// and matched with Interpreter.cpp::intUpdateCPUCycles().
			const bool lowcycles = (raw_cycles <= 40);
			const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
			u32 scale_cycles = 0;

			if (cyclerate == 0 || lowcycles || cyclerate < -99 || cyclerate > 3)
				scale_cycles = raw_cycles >> 3;
			else if (cyclerate > 1)
				scale_cycles = raw_cycles >> (2 + cyclerate);
			else if (cyclerate == 1)
				scale_cycles = (raw_cycles >> 3) / 1.3f;
			else if (cyclerate == -1)
				scale_cycles = (raw_cycles <= 80 || raw_cycles > 168 ? 5 : 7) * raw_cycles / 32;
			else
				scale_cycles = ((5 + (-2 * (cyclerate + 1))) * raw_cycles) >> 5;

			return (scale_cycles < 1) ? 1 : scale_cycles;
		}

		u32 RawCycleRemainderAfterClear(u32 raw_cycles)
		{
			// Ported from PCSX2 x86/ix86-32/iR5900.cpp::scaleblockcycles_clear().
			// The recompiler keeps the fixed-point remainder after an in-block
			// cycle commit, and the final block tail scales that remainder again.
			const bool lowcycles = (raw_cycles <= 40);
			const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
			if (!lowcycles && cyclerate > 1)
				return raw_cycles & ((0x1u << (cyclerate + 2)) - 1);

			return raw_cycles & 0x7u;
		}

		__noinline void VitaEeRaiseAddressError(u32 addr, bool store)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::RaiseAddressError().
			const std::string message(
				fmt::format("Address Error, addr=0x{:x} [{}]", addr, store ? "store" : "load"));
			Console.Error(message);
			Cpu->CancelInstruction();
		}

		__noinline void VitaEeMemReadCop1Word(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: FPU.cpp::LWC1().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuCop1MemoryHelperCalls;
#endif
			if (addr & 3)
			{
				Console.Error("FPU (LWC1 Opcode): Invalid Unaligned Memory Address");
				return;
			}

			fpuRegs.fpr[guest_reg].UL = memRead32(addr);
		}

		__noinline void VitaEeMemWriteCop1Word(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: FPU.cpp::SWC1().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuCop1MemoryHelperCalls;
#endif
			if (addr & 3)
			{
				Console.Error("FPU (SWC1 Opcode): Invalid Unaligned Memory Address");
				return;
			}

			memWrite32(addr, fpuRegs.fpr[guest_reg].UL);
		}

		__noinline void VitaEeDivSigned(u32 rs, u32 rt)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::DIV(). Cortex-A9 has no integer
			// divide instruction, so arbitrary division falls back here.
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuDivSignedHelperCalls;
#endif
			if (rs == 0x80000000u && rt == 0xffffffffu)
			{
				cpuRegs.LO.SD[0] = static_cast<s32>(0x80000000);
				cpuRegs.HI.SD[0] = 0;
			}
			else if (static_cast<s32>(rt) != 0)
			{
				cpuRegs.LO.SD[0] = static_cast<s32>(rs) / static_cast<s32>(rt);
				cpuRegs.HI.SD[0] = static_cast<s32>(rs) % static_cast<s32>(rt);
			}
			else
			{
				cpuRegs.LO.SD[0] = (static_cast<s32>(rs) < 0) ? 1 : -1;
				cpuRegs.HI.SD[0] = static_cast<s32>(rs);
			}
		}

		__noinline void VitaEeDivUnsigned(u32 rs, u32 rt)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::DIVU().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuDivUnsignedHelperCalls;
#endif
			if (rt != 0)
			{
				cpuRegs.LO.SD[0] = static_cast<s32>(rs / rt);
				cpuRegs.HI.SD[0] = static_cast<s32>(rs % rt);
			}
			else
			{
				cpuRegs.LO.SD[0] = -1;
				cpuRegs.HI.SD[0] = static_cast<s32>(rs);
			}
		}

		__noinline void VitaEeDivSigned1(u32 rs, u32 rt)
		{
			// PCSX2 owners: MMI.cpp::DIV1() and
			// x86/ix86-32/iR5900MultDiv.cpp::recDIV1().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuDivSigned1HelperCalls;
#endif
			if (rs == 0x80000000u && rt == 0xffffffffu)
			{
				cpuRegs.LO.SD[1] = static_cast<s32>(0x80000000);
				cpuRegs.HI.SD[1] = 0;
			}
			else if (static_cast<s32>(rt) != 0)
			{
				cpuRegs.LO.SD[1] = static_cast<s32>(rs) / static_cast<s32>(rt);
				cpuRegs.HI.SD[1] = static_cast<s32>(rs) % static_cast<s32>(rt);
			}
			else
			{
				cpuRegs.LO.SD[1] = (static_cast<s32>(rs) < 0) ? 1 : -1;
				cpuRegs.HI.SD[1] = static_cast<s32>(rs);
			}
		}

		__noinline void VitaEeDivUnsigned1(u32 rs, u32 rt)
		{
			// PCSX2 owners: MMI.cpp::DIVU1() and
			// x86/ix86-32/iR5900MultDiv.cpp::recDIVU1().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuDivUnsigned1HelperCalls;
#endif
			if (rt != 0)
			{
				cpuRegs.LO.SD[1] = static_cast<s32>(rs / rt);
				cpuRegs.HI.SD[1] = static_cast<s32>(rs % rt);
			}
			else
			{
				cpuRegs.LO.SD[1] = -1;
				cpuRegs.HI.SD[1] = static_cast<s32>(rs);
			}
		}

		__noinline void VitaEePackedDivSignedWords(u32 rs0, u32 rt0, u32 rs2, u32 rt2)
		{
			// PCSX2 owners: MMI.cpp::PDIVW() and x86/iMMI.cpp::recPDIVW().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuPackedDivSignedWordHelperCalls;
#endif
			const auto divide_lane = [](unsigned lane, u32 rs, u32 rt) {
				if (rs == 0x80000000u && rt == 0xffffffffu)
	{
					cpuRegs.LO.SD[lane] = static_cast<s32>(0x80000000);
					cpuRegs.HI.SD[lane] = 0;
	}
				else if (static_cast<s32>(rt) != 0)
	{
					cpuRegs.LO.SD[lane] = static_cast<s32>(rs) / static_cast<s32>(rt);
					cpuRegs.HI.SD[lane] = static_cast<s32>(rs) % static_cast<s32>(rt);
	}
				else
	{
					cpuRegs.LO.SD[lane] = (static_cast<s32>(rs) < 0) ? 1 : -1;
					cpuRegs.HI.SD[lane] = static_cast<s32>(rs);
	}
			};

			divide_lane(0, rs0, rt0);
			divide_lane(1, rs2, rt2);
		}

		__noinline void VitaEePackedDivUnsignedWords(u32 rs0, u32 rt0, u32 rs2, u32 rt2)
		{
			// PCSX2 owners: MMI.cpp::PDIVUW() and x86/iMMI.cpp::recPDIVUW().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuPackedDivUnsignedWordHelperCalls;
#endif
			const auto divide_lane = [](unsigned lane, u32 rs, u32 rt) {
				if (rt != 0)
	{
					cpuRegs.LO.SD[lane] = static_cast<s32>(rs / rt);
					cpuRegs.HI.SD[lane] = static_cast<s32>(rs % rt);
	}
				else
	{
					cpuRegs.LO.SD[lane] = -1;
					cpuRegs.HI.SD[lane] = static_cast<s32>(rs);
	}
			};

			divide_lane(0, rs0, rt0);
			divide_lane(1, rs2, rt2);
		}

		__noinline void VitaEePackedDivSignedWordsByHalfword(u32 op)
		{
			// PCSX2 owner: MMI.cpp::PDIVBW().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuPackedDivWordByHalfwordHelperCalls;
#endif
			const unsigned rs = RS(op);
			const unsigned rt = RT(op);
			const u16 raw_divisor = cpuRegs.GPR.r[rt].US[0];
			const s16 divisor = static_cast<s16>(raw_divisor);
			for (unsigned lane = 0; lane < 4; lane++)
			{
				const s32 dividend = cpuRegs.GPR.r[rs].SL[lane];
				if (static_cast<u32>(dividend) == 0x80000000u && raw_divisor == 0xffffu)
	{
					cpuRegs.LO.SL[lane] = static_cast<s32>(0x80000000);
					cpuRegs.HI.SL[lane] = 0;
	}
				else if (raw_divisor != 0)
	{
					cpuRegs.LO.SL[lane] = dividend / divisor;
					cpuRegs.HI.SL[lane] = dividend % divisor;
	}
				else
	{
					cpuRegs.LO.SL[lane] = (dividend < 0) ? 1 : -1;
					cpuRegs.HI.SL[lane] = dividend;
	}
			}
		}

	} // namespace

	static_assert(GprOffset(31) + sizeof(GPR_reg) <= 0x0fff);
	static_assert((GprOffset(0) % 16) == 0);
	static_assert((GprOffset(0) % alignof(u64)) == 0);
	static_assert(GprOffset(31) + sizeof(u64) <= 0x3fc);
	static_assert(HI_OFFSET + sizeof(GPR_reg) <= 0x0fff);
	static_assert(LO_OFFSET + sizeof(GPR_reg) <= 0x0fff);
	static_assert((HI_OFFSET % alignof(u64)) == 0);
	static_assert((LO_OFFSET % alignof(u64)) == 0);
	static_assert(Cp0Offset(31) + sizeof(u32) <= 0x0fff);
	static_assert(FprOffset(31) + sizeof(FPRreg) <= 0x0fff);
	static_assert(FprcOffset(31) + sizeof(u32) <= 0x0fff);
	static_assert(FPU_ACC_OFFSET + sizeof(FPRreg) <= 0x0fff);
	static_assert(FPU_ACCFLAG_OFFSET + sizeof(u32) <= 0x0fff);
	static_assert(SA_OFFSET + sizeof(u32) <= 0x0fff);
	static_assert(PC_OFFSET + sizeof(u32) <= 0x0fff);
	static_assert(CODE_OFFSET + sizeof(u32) <= 0x0fff);
	static_assert(PERF_OFFSET + sizeof(PERFregs) <= 0x0fff);
	static_assert(LAST_PERF_CYCLE_OFFSET + 2 * sizeof(u64) <= 0x0fff);
	static_assert((LAST_PERF_CYCLE_OFFSET % alignof(u64)) == 0);
	static_assert(CYCLE_OFFSET + sizeof(u64) <= 0x0fff);
	static_assert((CYCLE_OFFSET % alignof(u64)) == 0);
	static_assert(BRANCH_OFFSET + sizeof(int) <= 0x0fff);
	static_assert(NEXT_EVENT_OFFSET + sizeof(u64) <= 0x0fff);
	static_assert((NEXT_EVENT_OFFSET % alignof(u64)) == 0);
	static_assert(LAST_COP0_CYCLE_OFFSET + sizeof(u64) <= 0x0fff);
	static_assert((LAST_COP0_CYCLE_OFFSET % alignof(u64)) == 0);
	static_assert(TLB_ENTRY_COUNT == 48);
	static_assert(sizeof(vtlb_private::VTLBVirtual) == sizeof(u32));
	static_assert(TLB_ENTRY_SIZE == 16);
	static_assert(TLB_PAGE_MASK_OFFSET == 0);
	static_assert(TLB_ENTRY_HI_OFFSET == 4);
	static_assert(TLB_ENTRY_LO0_OFFSET == 8);
	static_assert(TLB_ENTRY_LO1_OFFSET == 12);
	static_assert(VU0_VF_OFFSET == 0);
	static_assert(VU0_VI_OFFSET + VU0_VI_STRIDE * 32 <= 0x0fff);
	static_assert(VU0_CLIPFLAG_OFFSET + sizeof(u32) <= 0x0fff);
#if !defined(VITASX2_QEMU_PROVIDER_FIXTURE)
	static_assert(VU0_MEM_OFFSET <= 0x0fff);
#endif

	void RefreshRawGpr0KnownZero()
	{
		s_raw_gpr0_known_zero = (cpuRegs.GPR.r[0].UD[0] == 0 && cpuRegs.GPR.r[0].UD[1] == 0) ? 1u : 0u;
	}

	BlockCompiler::BlockCompiler(VitaA32::CodeBuffer& code)
		: m_code(code)
	{
		m_branch_flag_host = HOST_BRANCH_FLAG;
		// Keep the allocator's dense logical q0-q7 domain while placing its upper
		// bank in AAPCS caller-clobbered d24-d31. Physical q8-q11 remain available
		// for the persistent COP2 normalization constants.
		m_code.SetNeonQRegisterBankMapping(4, 12, 4);
	}

	bool BlockCompiler::EmitAndImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags)
	{
		if (m_code.EmitAndImm32(rd, rn, value, set_flags) ||
			(!set_flags && m_code.EmitBicImm32(rd, rn, ~value)))
		{
			return true;
		}

		// UBFX is an exact one-instruction AND for a contiguous low mask when
		// APSR is not observed. This avoids materializing common 9-23-bit EE and
		// FPU masks which A32's modified-immediate encoding cannot represent.
		if (!set_flags && value != 0 && value != 0xffffffffu && (value & (value + 1u)) == 0)
		{
			u8 width = 0;
			for (u32 remaining = value; remaining != 0; remaining >>= 1)
				width++;

			if (m_code.EmitUbfx(rd, rn, 0, width))
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuAndLowMaskBitfieldFastPaths++;
#endif
				return true;
			}
		}

		return m_code.EmitMovImm32(scratch, value) && m_code.EmitAndReg(rd, rn, scratch, set_flags);
	}

	bool BlockCompiler::EmitOrrImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags)
	{
		return m_code.EmitOrrImm32(rd, rn, value, set_flags) ||
			   (m_code.EmitMovImm32(scratch, value) && m_code.EmitOrrReg(rd, rn, scratch, set_flags));
	}

	bool BlockCompiler::EmitEorImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags)
	{
		const bool immediate = m_code.EmitEorImm32(rd, rn, value, set_flags);
#if defined(VITASX2_QEMU_VALIDATION)
		if (immediate && !set_flags && value == 0xffffffffu)
			g_qemuEorAllOnesFastPaths++;
#endif
		return immediate ||
			(m_code.EmitMovImm32(scratch, value) && m_code.EmitEorReg(rd, rn, scratch, set_flags));
	}

	bool BlockCompiler::EmitBicImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags)
	{
		return m_code.EmitBicImm32(rd, rn, value, set_flags) ||
			   (!set_flags && m_code.EmitAndImm32(rd, rn, ~value)) ||
			   (m_code.EmitMovImm32(scratch, ~value) && m_code.EmitAndReg(rd, rn, scratch, set_flags));
	}

	bool BlockCompiler::EmitCmpImm32OrReg(unsigned rn, u32 value, unsigned scratch)
	{
		return EmitCmpImm32OrReg(rn, value, scratch, VitaA32::Condition::AL);
	}

	bool BlockCompiler::EmitCmpImm32OrReg(unsigned rn, u32 value, unsigned scratch,
		VitaA32::Condition condition)
	{
		return m_code.EmitCmpImm32(rn, value, condition) ||
			   (m_code.EmitMovImm32(scratch, value, condition) &&
			    m_code.EmitCmpReg(rn, scratch, condition));
	}

	bool BlockCompiler::CanCompileOpcode(u32 op)
	{
		switch (op >> 26)
		{
			case 0x00:
				return CanCompileSPECIAL(op);
			case 0x01:
				return CanCompileREGIMM(op);
			case 0x02: // J, owned by Interpreter.cpp::J().
			case 0x03: // JAL, owned by Interpreter.cpp::JAL().
				return true;
			case 0x04: // BEQ, owned by Interpreter.cpp::BEQ().
			case 0x05: // BNE, owned by Interpreter.cpp::BNE().
			case 0x06: // BLEZ, owned by Interpreter.cpp::BLEZ().
			case 0x07: // BGTZ, owned by Interpreter.cpp::BGTZ().
			case 0x14: // BEQL, owned by Interpreter.cpp::BEQL().
			case 0x15: // BNEL, owned by Interpreter.cpp::BNEL().
			case 0x16: // BLEZL, owned by Interpreter.cpp::BLEZL().
			case 0x17: // BGTZL, owned by Interpreter.cpp::BGTZL().
				return true;
			case 0x08: // ADDI, owned by R5900OpcodeImpl.cpp::ADDI().
			case 0x09: // ADDIU, owned by R5900OpcodeImpl.cpp::ADDIU().
			case 0x0a: // SLTI, owned by R5900OpcodeImpl.cpp::SLTI().
			case 0x0b: // SLTIU, owned by R5900OpcodeImpl.cpp::SLTIU().
			case 0x0c: // ANDI, owned by R5900OpcodeImpl.cpp::ANDI().
			case 0x0d: // ORI, owned by R5900OpcodeImpl.cpp::ORI().
			case 0x0e: // XORI, owned by R5900OpcodeImpl.cpp::XORI().
			case 0x0f: // LUI, owned by R5900OpcodeImpl.cpp::LUI().
				return true;
			case 0x10: // COP0 helper-backed system ops, owned by COP0.cpp and x86/iCOP0.cpp.
				return CanCompileCOP0(op);
			case 0x11: // COP1 helper-backed scalar/control ops, owned by FPU.cpp and x86/iFPU.cpp.
				return CanCompileCOP1(op);
			case 0x12: // COP2/VU0 macro interface, owned by COP2.cpp and VU0.cpp.
				return CanCompileCOP2(op);
			case 0x18: // DADDI, owned by R5900OpcodeImpl.cpp::DADDI().
			case 0x19: // DADDIU, owned by R5900OpcodeImpl.cpp::DADDIU().
			case 0x1a: // LDL, owned by R5900OpcodeImpl.cpp::LDL().
			case 0x1b: // LDR, owned by R5900OpcodeImpl.cpp::LDR().
			case 0x1e: // LQ, owned by R5900OpcodeImpl.cpp::LQ().
			case 0x1f: // SQ, owned by R5900OpcodeImpl.cpp::SQ().
			case 0x20: // LB, owned by R5900OpcodeImpl.cpp::LB().
			case 0x21: // LH, owned by R5900OpcodeImpl.cpp::LH().
			case 0x22: // LWL, owned by R5900OpcodeImpl.cpp::LWL().
			case 0x23: // LW, owned by R5900OpcodeImpl.cpp::LW().
			case 0x24: // LBU, owned by R5900OpcodeImpl.cpp::LBU().
			case 0x25: // LHU, owned by R5900OpcodeImpl.cpp::LHU().
			case 0x26: // LWR, owned by R5900OpcodeImpl.cpp::LWR().
			case 0x27: // LWU, owned by R5900OpcodeImpl.cpp::LWU().
			case 0x28: // SB, owned by R5900OpcodeImpl.cpp::SB().
			case 0x29: // SH, owned by R5900OpcodeImpl.cpp::SH().
			case 0x2a: // SWL, owned by R5900OpcodeImpl.cpp::SWL().
			case 0x2b: // SW, owned by R5900OpcodeImpl.cpp::SW().
			case 0x2c: // SDL, owned by R5900OpcodeImpl.cpp::SDL().
			case 0x2d: // SDR, owned by R5900OpcodeImpl.cpp::SDR().
			case 0x2e: // SWR, owned by R5900OpcodeImpl.cpp::SWR().
				return true;
			case 0x2f: // CACHE known modes, owned by Cache.cpp::CACHE().
				return CanCompileCACHE(op);
			case 0x31: // LWC1, owned by FPU.cpp::LWC1().
			case 0x33: // PREF, owned by R5900OpcodeImpl.cpp::PREF().
			case 0x36: // LQC2, owned by VU0.cpp::LQC2().
			case 0x37: // LD, owned by R5900OpcodeImpl.cpp::LD().
			case 0x39: // SWC1, owned by FPU.cpp::SWC1().
			case 0x3e: // SQC2, owned by VU0.cpp::SQC2().
			case 0x3f: // SD, owned by R5900OpcodeImpl.cpp::SD().
				return true;
			case 0x1c: // MMI scalar mult/div extensions, owned by MMI.cpp and iR5900MultDiv.cpp.
				return CanCompileMMI(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::IsSupportedBranchOpcode(u32 op)
	{
			switch (op >> 26)
			{
				case 0x00:
					switch (op & 0x3f)
	{
					case 0x08: // JR, owned by Interpreter.cpp::JR().
					case 0x09: // JALR, owned by Interpreter.cpp::JALR().
						return true;
					default:
						return false;
	}
			case 0x01:
				switch (RT(op))
	{
					case 0x00: // BLTZ, owned by Interpreter.cpp::BLTZ().
					case 0x01: // BGEZ, owned by Interpreter.cpp::BGEZ().
					case 0x02: // BLTZL, owned by Interpreter.cpp::BLTZL().
					case 0x03: // BGEZL, owned by Interpreter.cpp::BGEZL().
					case 0x10: // BLTZAL, owned by Interpreter.cpp::BLTZAL().
					case 0x11: // BGEZAL, owned by Interpreter.cpp::BGEZAL().
					case 0x12: // BLTZALL, owned by Interpreter.cpp::BLTZALL().
					case 0x13: // BGEZALL, owned by Interpreter.cpp::BGEZALL().
						return true;
					default:
						return false;
	}
			case 0x02: // J, owned by Interpreter.cpp::J().
			case 0x03: // JAL, owned by Interpreter.cpp::JAL().
				return true;
			case 0x04: // BEQ, owned by Interpreter.cpp::BEQ().
			case 0x05: // BNE, owned by Interpreter.cpp::BNE().
			case 0x06: // BLEZ, owned by Interpreter.cpp::BLEZ().
			case 0x07: // BGTZ, owned by Interpreter.cpp::BGTZ().
			case 0x14: // BEQL, owned by Interpreter.cpp::BEQL().
			case 0x15: // BNEL, owned by Interpreter.cpp::BNEL().
			case 0x16: // BLEZL, owned by Interpreter.cpp::BLEZL().
			case 0x17: // BGTZL, owned by Interpreter.cpp::BGTZL().
				return true;
			case 0x11: // COP1_BC1 branch forms, owned by FPU.cpp::BC1F()/BC1T()/BC1FL()/BC1TL().
				if (((op >> 21) & 0x1f) != 0x08)
					return false;
				switch (RT(op))
	{
					case 0x00:
					case 0x01:
					case 0x02:
					case 0x03:
						return true;
					default:
						return false;
	}
			case 0x12: // COP2_BC2 branch forms, owned by COP2.cpp::BC2F()/BC2T()/BC2FL()/BC2TL().
				return IsCOP2BranchOpcode(op);
			case 0x10: // COP0_BC0 branch forms, owned by COP0.cpp::BC0F()/BC0T()/BC0FL()/BC0TL().
				if (((op >> 21) & 0x1f) != 0x08)
					return false;
				switch (RT(op))
	{
					case 0x00:
					case 0x01:
					case 0x02:
					case 0x03:
						return true;
					default:
						return false;
	}
			default:
				return false;
			}
		}

	bool BlockCompiler::IsBranchLikely(u32 op)
	{
		// PCSX2 owners: Interpreter.cpp::BEQL()/BNEL()/BLEZL()/BGTZL() and the
		// REGIMM likely forms cancel the delay slot on the not-taken path.
		return IsBranchLikelyOpcode(op);
	}

	bool BlockCompiler::CanCompileDelaySlotOpcode(u32 op)
	{
		// PCSX2 x86/ix86-32/iR5900.cpp::recRecompile() detects branches in
		// delay slots through recompileNextInstruction(true, ...): the delay
		// branch is skipped as generated work and the outer branch still owns
		// the block exit.
		// SYSCALL/BREAK are different: R5900OpcodeImpl.cpp::SYSCALL()/BREAK()
		// are helper-backed exception paths, and Interpreter.cpp::_doBranch_shared()
		// marks cpuRegs.branch before executing them as delay slots.
		// Trap ops use the same exception-shaped helper/event tail.
		return CanCompileOpcode(op) && (op >> 26) != 0x12 && !IsDI(op) && !IsInBlockTLBReadProbe(op) &&
			   (!RequiresBlockEndAfterOpcode(op) || IsSYSCALL(op) || IsBREAK(op) ||
				   IsTrapOpcode(op) || IsCounterReadLoad(op));
	}

	bool BlockCompiler::RequiresBlockEndAfterOpcode(u32 op)
	{
		// R5900OpcodeImpl.cpp::SYNC() is a no-op, but local EE docs still forbid
		// compiling it inside a branch delay slot, so make it a one-op tail.
		// Counter-read narrow loads (LB/LH/LW/LBU/LHU) do NOT end blocks: the
		// PCSX2 x86 comparison point iR5900LoadStore.cpp::recLoad() only ends
		// the block for a compile-time-constant counter-page address, and the
		// A32 handler cold tail already ports R5900OpcodeImpl.cpp's runtime
		// counter check as a mid-block cycle-committing event exit at pc + 4,
		// so loads continue straight-line blocks like PCSX2 x86 blocks do.
		switch (op >> 26)
		{
			case 0x00:
				return (op & 0x3f) == 0x0c || (op & 0x3f) == 0x0d ||
					   (op & 0x3f) == 0x0f || IsSpecialTrap(op);
			case 0x01:
				return IsRegImmTrap(op);
			case 0x10:
				return CanCompileCOP0(op) && !IsDI(op) && !IsFastMFC0(op) && !IsFastMTC0(op) &&
					   !IsInBlockTLBReadProbe(op);
			case 0x11:
				return CanCompileCOP1(op) && !IsFastCOP1InBlock(op);
			case 0x12:
				return CanCompileCOP2(op) && !IsFastCOP2InBlock(op);
			case 0x2f:
				return IsHelperCACHE(op);
			default:
				return false;
		}
	}

	bool OpcodeMayUseVtlbFastPath(u32 op)
	{
		switch (op >> 26)
		{
			case 0x1a: // LDL
			case 0x1b: // LDR
			case 0x1e: // LQ
			case 0x1f: // SQ
			case 0x20: // LB
			case 0x21: // LH
			case 0x22: // LWL
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
			case 0x26: // LWR
			case 0x27: // LWU
			case 0x28: // SB
			case 0x29: // SH
			case 0x2a: // SWL
			case 0x2b: // SW
			case 0x2c: // SDL
			case 0x2d: // SDR
			case 0x2e: // SWR
			case 0x31: // LWC1
			case 0x36: // LQC2
			case 0x37: // LD
			case 0x39: // SWC1
			case 0x3e: // SQC2
			case 0x3f: // SD
				return true;
			default:
				return false;
		}
	}

	bool KnownVtlbNonHandlerAddress(u32 guest_addr)
	{
		if (!vtlb_private::vtlbdata.vmap)
			return false;

		const vtlb_private::VTLBVirtual vmv =
			vtlb_private::vtlbdata.vmap[guest_addr >> vtlb_private::VTLB_PAGE_BITS];
		return !vmv.isHandler(guest_addr);
	}

	bool KnownAddressUsesCounterReadEvent(u32 guest_addr)
	{
		return (guest_addr & 0xffffe000u) == 0x10000000u;
	}

	bool OpcodeUsesKnownVtlbNonHandlerAddress(u32 op, u32 known_address)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memRead*()/vtlb_memWrite*().
		// Mirror the known-address direct paths below so blocks that only emit
		// vmv.assumePtr() references don't reserve or load runtime VTLB bases.
		switch (op >> 26)
		{
			case 0x1a: // LDL
			case 0x1b: // LDR
			case 0x2c: // SDL
			case 0x2d: // SDR
				return KnownVtlbNonHandlerAddress(known_address & ~7u);

			case 0x1e: // LQ
			case 0x1f: // SQ
				return KnownVtlbNonHandlerAddress(known_address & ~0x0fu);

			case 0x20: // LB
			case 0x24: // LBU
				return !KnownAddressUsesCounterReadEvent(known_address) &&
					   KnownVtlbNonHandlerAddress(known_address);

			case 0x21: // LH
			case 0x25: // LHU
				return (known_address & 1u) == 0 &&
					   !KnownAddressUsesCounterReadEvent(known_address) &&
					   KnownVtlbNonHandlerAddress(known_address);

			case 0x22: // LWL
			case 0x26: // LWR
			case 0x2a: // SWL
			case 0x2e: // SWR
				return KnownVtlbNonHandlerAddress(known_address & ~3u);

			case 0x23: // LW
				return (known_address & 3u) == 0 &&
					   !KnownAddressUsesCounterReadEvent(known_address) &&
					   KnownVtlbNonHandlerAddress(known_address);

			case 0x27: // LWU
				return (known_address & 3u) == 0 &&
					   KnownVtlbNonHandlerAddress(known_address);

			case 0x28: // SB
				return KnownVtlbNonHandlerAddress(known_address);

			case 0x29: // SH
				return (known_address & 1u) == 0 &&
					   KnownVtlbNonHandlerAddress(known_address);

			case 0x2b: // SW
			case 0x31: // LWC1
			case 0x39: // SWC1
				return (known_address & 3u) == 0 &&
					   KnownVtlbNonHandlerAddress(known_address);

			case 0x36: // LQC2
			case 0x3e: // SQC2
				return KnownVtlbNonHandlerAddress(known_address);

			case 0x37: // LD
			case 0x3f: // SD
				return (known_address & 7u) == 0 &&
					   KnownVtlbNonHandlerAddress(known_address);

			default:
				return false;
		}
	}

	namespace
	{
		struct GprPinOpInfo
		{
			u8 low_reads[4]{};
			unsigned low_read_count = 0;
			u8 dword_reads[4]{};
			unsigned dword_read_count = 0;
		};

		struct DirtyGprPinOpInfo
		{
			u8 writes[2]{};
			unsigned write_count = 0;
		};
	} // namespace

	// Classifies one accepted EE opcode for the write-through GPR pin cache.
	// Returns true when every GPR-file write the op can perform goes through
	// EmitStoreGpr64()/EmitStoreGprZero64()/EmitStoreGprLowPreserveHigh(),
	// EmitStoreGprWord(), EmitStoreGprQ128(), EmitStoreGprQ128ToAddress(), or
	// EmitStoreGprDwordPair().
	// Returns false for op classes whose write paths are not certified yet;
	// those blocks compile without pins. The read lists score low32 and
	// low64 sources that reach EmitLoadGprLow()/EmitLoadGpr64().
	static bool ClassifyOpcodeForGprPinning(u32 op, GprPinOpInfo* info)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		const auto add_low_read = [info](unsigned guest_reg) {
			if (guest_reg != 0 && info->low_read_count < 4)
				info->low_reads[info->low_read_count++] = static_cast<u8>(guest_reg);
		};
		const auto add_dword_read = [info](unsigned guest_reg) {
			if (guest_reg != 0 && info->dword_read_count < 4)
				info->dword_reads[info->dword_read_count++] = static_cast<u8>(guest_reg);
		};

		switch (op >> 26)
		{
			case 0x00:
				switch (op & 0x3f)
	{
					case 0x00: // SLL
					case 0x02: // SRL
					case 0x03: // SRA
						add_low_read(rt);
						return true;
					case 0x38: // DSLL
					case 0x3a: // DSRL
					case 0x3b: // DSRA
					case 0x3c: // DSLL32
					case 0x3e: // DSRL32
					case 0x3f: // DSRA32
						add_dword_read(rt);
						return true;
					case 0x04: // SLLV
					case 0x06: // SRLV
					case 0x07: // SRAV
						add_low_read(rt);
						add_low_read(rs);
						return true;
					case 0x14: // DSLLV
					case 0x16: // DSRLV
					case 0x17: // DSRAV
						add_dword_read(rt);
						add_low_read(rs);
						return true;
					case 0x08: // JR
					case 0x09: // JALR links through the seam
						add_low_read(rs);
						return true;
					case 0x18: // MULT
					case 0x19: // MULTU
					case 0x1a: // DIV
					case 0x1b: // DIVU
					case 0x20: // ADD
					case 0x21: // ADDU
					case 0x22: // SUB
					case 0x23: // SUBU
					case 0x30: // TGE
					case 0x31: // TGEU
					case 0x32: // TLT
					case 0x33: // TLTU
					case 0x34: // TEQ
					case 0x36: // TNE
						add_low_read(rs);
						add_low_read(rt);
						return true;
					case 0x0a: // MOVZ
					case 0x0b: // MOVN
					case 0x24: // AND
					case 0x25: // OR
					case 0x26: // XOR
					case 0x27: // NOR
					case 0x2a: // SLT
					case 0x2b: // SLTU
					case 0x2c: // DADD
					case 0x2d: // DADDU
					case 0x2e: // DSUB
					case 0x2f: // DSUBU
						add_dword_read(rs);
						add_dword_read(rt);
						return true;
					case 0x0c: // SYSCALL exits the block through the event helper
					case 0x0d: // BREAK exits the block through the event helper
					case 0x0f: // SYNC
					case 0x10: // MFHI writes rd through the seam
					case 0x12: // MFLO writes rd through the seam
					case 0x28: // MFSA writes rd through the seam
						return true;
					case 0x11: // MTHI
					case 0x13: // MTLO
						add_dword_read(rs);
						return true;
					case 0x29: // MTSA
						add_low_read(rs);
						return true;
					default:
						return false;
	}
			case 0x01:
				switch (rt)
	{
					case 0x00: // BLTZ
					case 0x01: // BGEZ
					case 0x02: // BLTZL
					case 0x03: // BGEZL
					case 0x08: // TGEI
					case 0x09: // TGEIU
					case 0x0a: // TLTI
					case 0x0b: // TLTIU
					case 0x0c: // TEQI
					case 0x0e: // TNEI
					case 0x10: // BLTZAL links through the seam
					case 0x11: // BGEZAL links through the seam
					case 0x12: // BLTZALL links through the seam
					case 0x13: // BGEZALL links through the seam
						add_dword_read(rs);
						return true;
					case 0x18: // MTSAB
					case 0x19: // MTSAH
						add_low_read(rs);
						return true;
					default:
						return false;
	}
			case 0x02: // J
			case 0x03: // JAL links through the seam
			case 0x0f: // LUI
			case 0x2f: // CACHE writes no GPR
			case 0x33: // PREF
				return true;
			case 0x10: // COP0, owned by COP0.cpp and x86/iCOP0.cpp.
				switch ((op >> 21) & 0x1f)
	{
					case 0x00: // MFC0 writes rt through EmitStoreGpr64().
						return IsFastMFC0(op);
					case 0x04: // MTC0 writes CP0; active stores read rt through EmitLoadGprLow().
						if (!IsFastMTC0(op))
							return false;
						if (rd == 0x18 || (rd == 0x19 && (op & 1u) == 0 && (op & 0x3eu) != 0))
							return true;
						add_low_read(rt);
						return true;
					case 0x08: // COP0_BC0 branch forms read only CPCOND0.
						return CanCompileCOP0(op);
					case 0x10: // COP0_C0 system/TLB ops write no GPR; block-ending helpers exit immediately.
						return CanCompileCOP0(op);
					default:
						return false;
	}
			case 0x11: // COP1, owned by FPU.cpp and x86/iFPU.cpp.
				if (((op >> 21) & 0x1f) == 0x08)
					return CanCompileCOP1(op); // COP1_BC1 branches read only FCR31.C.

				if (!IsFastCOP1InBlock(op))
					return CanCompileCOP1(op); // Accepted helper fallbacks are block-ending event tails.

				if (((op >> 21) & 0x1f) == 0x04 ||
					(((op >> 21) & 0x1f) == 0x06 && rd == 31))
	{
					add_low_read(rt); // MTC1/CTC1 read rt through EmitLoadGprLow().
	}
				return true;
			case 0x12: // COP2/VU0, owned by VU0.cpp and x86/microVU_Macro.inl.
				if (IsCOP2BranchOpcode(op))
					return true;

				if (IsFastCOP2VectorTransfer(op))
	{
					return true;
	}

				if (IsFastCOP2ControlRead(op))
					return true; // CFC2 uses EmitStoreGpr64() or EmitStoreGprLowPreserveHigh().

				if (IsFastCOP2ControlWrite(op))
	{
					switch (rd)
	{
						case 0:
						case VU0_REG_MAC_FLAG:
						case VU0_REG_TPC:
						case VU0_REG_VPU_STAT:
							return true;
						default:
							add_low_read(rt); // CTC2 active writes read nonzero rt through the pin-aware raw-zero loader.
							return true;
	}
	}

				return CanCompileCOP2(op); // Remaining helper-backed macro forms are block-ending event tails.
			case 0x04: // BEQ
			case 0x05: // BNE
			case 0x14: // BEQL
			case 0x15: // BNEL
				add_dword_read(rs);
				add_dword_read(rt);
				return true;
			case 0x06: // BLEZ
			case 0x07: // BGTZ
			case 0x16: // BLEZL
			case 0x17: // BGTZL
				add_dword_read(rs);
				return true;
			case 0x08: // ADDI
			case 0x09: // ADDIU
			case 0x0c: // ANDI
				add_low_read(rs);
				return true;
			case 0x0a: // SLTI
			case 0x0b: // SLTIU
			case 0x0d: // ORI
			case 0x0e: // XORI
			case 0x18: // DADDI
			case 0x19: // DADDIU
				add_dword_read(rs);
				return true;
			case 0x1a: // LDL writes byte lanes then refreshes the low-word pin from backing.
			case 0x1b: // LDR writes byte lanes then refreshes the low-word pin from backing.
				add_low_read(rs);
				return true;
			case 0x1e: // LQ writes rt through EmitStoreGprQ128() and refreshes the cold-tail pin.
				add_low_read(rs);
				return true;
			case 0x22: // LWL merges rt through EmitStoreGpr64() and refreshes the cold-tail pin.
			case 0x26: // LWR merges rt through EmitStoreGpr64() and refreshes the cold-tail pin.
				add_low_read(rs);
				add_low_read(rt);
				return true;
			case 0x1c:
				switch (op & 0x3f)
	{
					case 0x00: // MADD
					case 0x01: // MADDU
					case 0x18: // MULT1
					case 0x19: // MULTU1
					case 0x1a: // DIV1
					case 0x1b: // DIVU1
					case 0x20: // MADD1
					case 0x21: // MADDU1
						add_low_read(rs);
						add_low_read(rt);
						return true;
					case 0x04: // PLZCW reads RS.UL[0]/[1] and writes rd through EmitStoreGprWord().
						add_dword_read(rs);
						return true;
					case 0x10: // MFHI1 writes rd through the seam
					case 0x12: // MFLO1 writes rd through the seam
						return true;
					case 0x11: // MTHI1
					case 0x13: // MTLO1
						add_dword_read(rs);
						return true;
					case 0x30: // PMFHL writes rd from HI/LO through EmitStoreGprWord().
						return true;
					case 0x31: // PMTHL reads RS words into HI/LO
						add_low_read(rs);
						return true;
					case 0x34: // PSLLH
					case 0x36: // PSRLH
					case 0x37: // PSRAH
					case 0x3c: // PSLLW
					case 0x3e: // PSRLW
					case 0x3f: // PSRAW
						return true; // Packed immediate shifts write rd through EmitStoreGprQ128().
					case 0x08: // MMI0 class, owned by R5900OpcodeTables.cpp::Class_MMI0().
	{
						if (!CanCompileMMI0(op))
							return false;

						switch ((op >> 6) & 0x1f)
						{
							default:
								break;
						}
						return true;
	}
					case 0x09: // MMI2 class, owned by R5900OpcodeTables.cpp::Class_MMI2().
	{
						if (!CanCompileMMI2(op))
							return false;

							switch ((op >> 6) & 0x1f)
							{
								case 0x02: // PSLLVW reads RT through NEON and RS word shift counts.
								case 0x03: // PSRLVW reads RT through NEON and RS word shift counts.
									add_low_read(rs);
									break;
							case 0x0c: // PMULTW reads full RS/RT qwords through NEON.
								break;
							case 0x10: // PMADDH reads full RS/RT qwords through NEON.
							case 0x14: // PMSUBH reads full RS/RT qwords through NEON.
								break;
							case 0x11: // PHMADH reads full RS/RT qwords through NEON.
							case 0x15: // PHMSBH reads full RS/RT qwords through NEON.
								break;
							case 0x1c: // PMULTH reads full RS/RT qwords through NEON.
								break;
							case 0x00: // PMADDW
								case 0x04: // PMSUBW
								case 0x0d: // PDIVW
								case 0x1d: // PDIVBW
									add_low_read(rs);
									add_low_read(rt);
									break;
								case 0x1a: // PEXEH
								case 0x1e: // PEXEW
								case 0x1f: // PROT3W
									add_low_read(rt);
									break;
							default:
								break;
						}
						return true;
	}
					case 0x28: // MMI1 class, owned by R5900OpcodeTables.cpp::Class_MMI1().
						if (!CanCompileMMI1(op))
							return false;

						return true;
					case 0x29: // MMI3 class, owned by R5900OpcodeTables.cpp::Class_MMI3().
	{
						if (!CanCompileMMI3(op))
							return false;

						switch ((op >> 6) & 0x1f)
						{
							case 0x08: // PMTHI reads RS into HI
							case 0x09: // PMTLO reads RS into LO
								// Full-qword NEON copies still read authoritative memory;
								// don't score them as low32 pin users until q-register
								// materialization is certified.
								return true;
							default:
								break;
						}

							switch ((op >> 6) & 0x1f)
							{
								case 0x03: // PSRAVW reads RT through NEON and RS word shift counts.
									add_low_read(rs);
									break;
							case 0x00: // PMADDUW reads full RS/RT qwords through NEON.
								break;
							case 0x0c: // PMULTUW reads full RS/RT qwords through NEON.
								break;
								case 0x0d: // PDIVUW
									add_low_read(rs);
									add_low_read(rt);
									break;
								case 0x1e: // PEXCW
									add_low_read(rt);
									break;
							default:
								break;
						}
						return true;
	}
					default:
						return false;
	}
			case 0x20: // LB
			case 0x21: // LH
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
			case 0x27: // LWU
			case 0x37: // LD writes rt through the seam in hot and cold paths
			case 0x31: // LWC1 writes an FPR only
			case 0x39: // SWC1
			case 0x36: // LQC2 writes a VU0 register only
			case 0x3e: // SQC2
				add_low_read(rs);
				return true;
			case 0x28: // SB
			case 0x29: // SH
			case 0x2b: // SW
			case 0x3f: // SD
			case 0x2a: // SWL writes guest RAM only
			case 0x2c: // SDL writes guest RAM only
			case 0x2d: // SDR writes guest RAM only
			case 0x2e: // SWR writes guest RAM only
			case 0x1f: // SQ writes guest RAM only
				add_low_read(rs);
				add_low_read(rt);
				return true;
			default:
				return false;
		}
	}

	static bool ClassifyOpcodeForDirtyGprPins(u32 op, DirtyGprPinOpInfo* info)
	{
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		const auto add_write = [info](unsigned guest_reg) {
			if (guest_reg != 0 && info->write_count < 2)
				info->writes[info->write_count++] = static_cast<u8>(guest_reg);
		};

		switch (op >> 26)
		{
			case 0x00:
				switch (op & 0x3f)
	{
					case 0x00: // SLL
					case 0x02: // SRL
					case 0x03: // SRA
					case 0x04: // SLLV
					case 0x06: // SRLV
					case 0x07: // SRAV
					case 0x0a: // MOVZ
					case 0x0b: // MOVN
					case 0x14: // DSLLV
					case 0x16: // DSRLV
					case 0x17: // DSRAV
					case 0x20: // ADD
					case 0x21: // ADDU
					case 0x22: // SUB
					case 0x23: // SUBU
					case 0x24: // AND
					case 0x25: // OR
					case 0x26: // XOR
					case 0x27: // NOR
					case 0x2a: // SLT
					case 0x2b: // SLTU
					case 0x2c: // DADD
					case 0x2d: // DADDU
					case 0x2e: // DSUB
					case 0x2f: // DSUBU
					case 0x38: // DSLL
					case 0x3a: // DSRL
					case 0x3b: // DSRA
					case 0x3c: // DSLL32
					case 0x3e: // DSRL32
					case 0x3f: // DSRA32
						add_write(rd);
						return true;
					case 0x08: // JR
					case 0x0f: // SYNC
						return true;
					case 0x09: // JALR
						add_write(rd);
						return true;
					case 0x0c: // SYSCALL syncs pins in its block-ending exception tail.
					case 0x0d: // BREAK syncs pins in its block-ending exception tail.
					case 0x30: // TGE
					case 0x31: // TGEU
					case 0x32: // TLT
					case 0x33: // TLTU
					case 0x34: // TEQ
					case 0x36: // TNE all sync pins in the block-ending trap tail.
						return true;
					case 0x10: // MFHI writes rd through EmitStoreGpr64().
					case 0x12: // MFLO writes rd through EmitStoreGpr64().
					case 0x18: // MULT writes rd through EmitStoreGpr64(); LO/HI are not GPR-file state.
					case 0x19: // MULTU writes rd through EmitStoreGpr64().
					case 0x28: // MFSA writes rd through EmitStoreGprZeroExtended32FromLow().
						add_write(rd);
						return true;
					case 0x11: // MTHI reads rs pin-aware and writes only HI.
					case 0x13: // MTLO reads rs pin-aware and writes only LO.
					case 0x1a: // DIV is fully inline and writes only LO/HI.
					case 0x1b: // DIVU is fully inline and writes only LO/HI.
					case 0x29: // MTSA reads rs pin-aware and writes only cpuRegs.sa.
						return true;
					default:
						return false;
	}
			case 0x01:
				switch (rt)
	{
					case 0x00: // BLTZ
					case 0x01: // BGEZ
					case 0x02: // BLTZL
					case 0x03: // BGEZL
						return true;
					case 0x10: // BLTZAL
					case 0x11: // BGEZAL
					case 0x12: // BLTZALL
					case 0x13: // BGEZALL
						add_write(31);
						return true;
					case 0x08: // TGEI
					case 0x09: // TGEIU
					case 0x0a: // TLTI
					case 0x0b: // TLTIU
					case 0x0c: // TEQI
					case 0x0e: // TNEI all sync pins in the block-ending trap tail.
						return true;
					case 0x18: // MTSAB reads rs pin-aware and writes only cpuRegs.sa.
					case 0x19: // MTSAH reads rs pin-aware and writes only cpuRegs.sa.
						return true;
					default:
						return false;
	}
			case 0x02: // J
				return true;
			case 0x03: // JAL
				add_write(31);
				return true;
			case 0x2f: // CACHE reads rs pin-aware; executeCacheOp() touches only
				// the cache model and guest memory, never the GPR file.
			case 0x33: // PREF is a PCSX2 no-op.
				return true;
			case 0x04: // BEQ
			case 0x05: // BNE
			case 0x06: // BLEZ
			case 0x07: // BGTZ
			case 0x14: // BEQL
			case 0x15: // BNEL
			case 0x16: // BLEZL
			case 0x17: // BGTZL
				return true;
			case 0x08: // ADDI
			case 0x09: // ADDIU
			case 0x0a: // SLTI
			case 0x0b: // SLTIU
			case 0x0c: // ANDI
			case 0x0d: // ORI
			case 0x0e: // XORI
			case 0x0f: // LUI
			case 0x18: // DADDI
			case 0x19: // DADDIU
				add_write(rt);
				return true;
			case 0x1c:
				switch (op & 0x3f)
	{
					case 0x00: // MADD writes rd through EmitStoreGpr64().
					case 0x01: // MADDU writes rd through EmitStoreGpr64().
					case 0x04: // PLZCW writes rd words 0/1 through EmitStoreGprWord().
					case 0x10: // MFHI1 writes rd through EmitStoreGpr64().
					case 0x12: // MFLO1 writes rd through EmitStoreGpr64().
					case 0x18: // MULT1 writes rd through EmitStoreGpr64().
					case 0x19: // MULTU1 writes rd through EmitStoreGpr64().
					case 0x20: // MADD1 writes rd through EmitStoreGpr64().
					case 0x21: // MADDU1 writes rd through EmitStoreGpr64().
						add_write(rd);
						return true;
					case 0x11: // MTHI1 reads rs pin-aware and writes only HI.
					case 0x13: // MTLO1 reads rs pin-aware and writes only LO.
					case 0x1a: // DIV1 is fully inline and writes only LO/HI.
					case 0x1b: // DIVU1 is fully inline and writes only LO/HI.
						return true;
					default:
						return false;
	}
			case 0x20: // LB
			case 0x21: // LH
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
			case 0x27: // LWU
			case 0x37: // LD
				add_write(rt);
				return true;
			case 0x10: // COP0, owned by COP0.cpp and x86/iCOP0.cpp.
				switch ((op >> 21) & 0x1f)
	{
					case 0x00: // MFC0 fast forms write rt through the extended-store
						// seam; helper fallbacks sync pins and end the block.
						if (!IsFastMFC0(op))
							return CanCompileCOP0(op);
						add_write(rt);
						return true;
					case 0x04: // MTC0 fast forms read rt pin-aware and write CP0/perf
						// state only; helper fallbacks sync pins and end the block.
						if (!IsFastMTC0(op))
							return CanCompileCOP0(op);
						return true;
					case 0x08: // COP0_BC0 branch forms read only CPCOND0.
						return CanCompileCOP0(op);
					case 0x10: // COP0_C0: in-block TLBR/TLBP/DI write CP0 only; EI and
						// ERET flush dirty pins before their event exits, and the
						// remaining helper tails sync pins and end the block.
						return CanCompileCOP0(op);
					default:
						return false;
	}
			case 0x11: // COP1, owned by FPU.cpp and x86/iFPU.cpp.
				if (((op >> 21) & 0x1f) == 0x08)
					return CanCompileCOP1(op); // BC1 reads only FCR31.C.
				if (!IsFastCOP1InBlock(op))
	{
					// Accepted helper fallbacks are block-ending event tails that
					// sync pins through EmitSystemHelperEventExit().
					return CanCompileCOP1(op);
	}
				if (((op >> 21) & 0x1f) == 0x00 || ((op >> 21) & 0x1f) == 0x02)
					add_write(rt); // MFC1/CFC1 write rt through the extended-store seam.
				return true;
			case 0x1a: // LDL merges through pin-aware value reads when rt is pinned,
			case 0x1b: // LDR through raw backing otherwise; the cold tail syncs pins
				// before its byte merge and refreshes the rt pin afterwards.
			case 0x22: // LWL merges rt through the pin-aware loaders and the
			case 0x26: // LWR deferring extended-store seam; the cold tail syncs
				// pins and flushes tail deferrals before rejoining.
				add_write(rt);
				return true;
			case 0x1e: // LQ writes rt through the pin-updating EmitStoreGprQ128().
			case 0x1f: // SQ either stores a resident qcache qreg directly or reads
				// rt through EmitLoadGprQ128(), which flushes deferred pinned words
				// before touching the raw backing slot; the cold tails sync pins
				// before their vtlb helpers.
				return true;
			case 0x2a: // SWL reads rt through the pin-aware partial-store loader.
			case 0x2e: // SWR reads rt through the pin-aware partial-store loader.
			case 0x2c: // SDL flushes rt's deferred pinned words before its
			case 0x2d: // SDR byte-lane backing reads; tails sync pins first.
				return true;
			case 0x31: // LWC1 writes an FPR only; the cold tail syncs pins before memRead32().
			case 0x39: // SWC1 reads an FPR only; the cold tail syncs pins before memWrite32().
				return true;
			case 0x28: // SB
			case 0x29: // SH
			case 0x2b: // SW
			case 0x3f: // SD
				return true;
			default:
				return false;
			}
		}

	bool BlockCanUseDirtyGprPins(u32 start_pc, u32 instruction_count)
	{
		if (instruction_count == 0 || EmuConfig.Gamefixes.GoemonTlbHack)
			return false;

#if !defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		if (Pcsx2Trace::IsGsTraceEnabled() || Pcsx2Trace::IsVuTraceEnabled())
			return false;
#endif

		for (u32 i = 0; i < instruction_count; i++)
		{
			DirtyGprPinOpInfo info;
			if (!ClassifyOpcodeForDirtyGprPins(memRead32(start_pc + i * 4), &info))
				return false;
		}

		return true;
	}

	bool GprLinkSignature::IsValid() const
	{
		if (count == 0 || count > MAX_PINS || block_pcs[0] >= block_pcs[1] ||
			(block_pcs[0] & 3u) != 0 || (block_pcs[1] & 3u) != 0 ||
			(scheduler.IsValid() && scheduler.host != SCHEDULER_HOST) ||
			(vtlb_pointer.IsValid() &&
				(vtlb_pointer.host != VTLB_POINTER_HOST ||
				 vtlb_pointer.guest_address == 0 || vtlb_pointer.stride == 0 ||
				 (vtlb_pointer.access_pc != block_pcs[0] &&
				  vtlb_pointer.access_pc != block_pcs[1]) ||
				 (vtlb_pointer.advance_pc != block_pcs[0] &&
				  vtlb_pointer.advance_pc != block_pcs[1]))))
		{
			return false;
		}

		u16 host_mask = 0;
		const auto valid_mapping_host = [](u8 host) {
			return host == 1 || host == 3 ||
				(host >= FIRST_HOST && host <= LAST_CALLEE_HOST) ||
				host == LINK_REGISTER_HOST;
		};
		const auto default_mapping_host = [](u8 host) {
			return host >= DEFAULT_FIRST_HOST && host <= LAST_CALLEE_HOST;
		};
		for (u8 i = 0; i < count; i++)
		{
			const GprLinkMapping& mapping = mappings[i];
			if (mapping.guest == 0 || mapping.guest >= 32 ||
				!valid_mapping_host(mapping.low_host))
			{
				return false;
			}
			if ((!default_mapping_host(mapping.low_host) ||
				(mapping.width == GprLinkWidth::Low64 &&
				 !default_mapping_host(mapping.high_host))) &&
				!vtlb_pointer.IsValid())
			{
				return false;
			}
			for (u8 previous = 0; previous < i; previous++)
			{
				if (mappings[previous].guest == mapping.guest)
					return false;
			}
			const u16 low_host_bit = static_cast<u16>(1u << mapping.low_host);
			if ((host_mask & low_host_bit) != 0)
				return false;
			host_mask |= low_host_bit;
			if (mapping.width == GprLinkWidth::Low64)
			{
				if (!valid_mapping_host(mapping.high_host) ||
					mapping.high_host == mapping.low_host)
				{
					return false;
				}
				const u16 high_host_bit = static_cast<u16>(1u << mapping.high_host);
				if ((host_mask & high_host_bit) != 0)
					return false;
				host_mask |= high_host_bit;
			}
			else if (mapping.high_host != GprLinkMapping::NO_HOST)
			{
				return false;
			}
		}
		return true;
	}

	bool GprLinkSignature::ContainsPc(u32 pc) const
	{
		return IsValid() && (block_pcs[0] == pc || block_pcs[1] == pc);
	}

	bool GprLinkSignature::HasWriteBack() const
	{
		for (u8 i = 0; i < count; i++)
		{
			if (mappings[i].dirty == GprLinkDirtyState::WriteBack)
				return true;
		}
		return false;
	}

	bool GprLinkSignature::ReclaimsVtlbHosts() const
	{
		if (!vtlb_pointer.IsValid())
			return false;
		for (u8 i = 0; i < count; i++)
		{
			if (mappings[i].low_host < DEFAULT_FIRST_HOST ||
				mappings[i].low_host > LAST_CALLEE_HOST ||
				(mappings[i].width == GprLinkWidth::Low64 &&
				 (mappings[i].high_host < DEFAULT_FIRST_HOST ||
				  mappings[i].high_host > LAST_CALLEE_HOST)))
			{
				return true;
			}
		}
		return false;
	}

	u8 GprLinkSignature::WordCount() const
	{
		u8 words = 0;
		for (u8 i = 0; i < count; i++)
			words += mappings[i].width == GprLinkWidth::Low64 ? 2 : 1;
		return words;
	}

	u8 GprLinkSignature::DirtyWordCount() const
	{
		u8 words = 0;
		for (u8 i = 0; i < count; i++)
		{
			if (mappings[i].dirty == GprLinkDirtyState::WriteBack)
				words += mappings[i].width == GprLinkWidth::Low64 ? 2 : 1;
		}
		return words;
	}

	bool BlockCompiler::BuildGprLinkSignature(u32 first_pc,
		u32 first_instruction_count, u32 second_pc, u32 second_instruction_count,
		GprLinkSignature* signature, bool reclaim_vtlb_hosts)
	{
		if (!signature)
			return false;
		*signature = GprLinkSignature{};
		if (!BlockCanUseDirtyGprPins(first_pc, first_instruction_count) ||
			!BlockCanUseDirtyGprPins(second_pc, second_instruction_count))
		{
			return false;
		}

		// PCSX2 owners: x86/ix86-32/iCore.cpp::_allocX86reg() and
		// x86/iCore.cpp::_clearNeededXMMregs() define persistent
		// MODE_READ/MODE_WRITE mappings within compiled code, while
		// x86/BaseblockEx.cpp::BaseBlocks::Link() owns reversible direct edges.
		// Adapt those contracts into one deterministic width-aware mapping from
		// the combined two-block use counts. Ordinary chains use r9-r11; a proven
		// translated-pointer chain may also reclaim r7/r8 because it materializes
		// vTLB bases only on cold translation and restores the dispatcher ABI on
		// every external exit. Reject COP1/COP2 blocks which reserve r10/r11 for
		// their private fast-path constants.
		u16 scores[32]{};
		u16 dword_scores[32]{};
		u16 writes[32]{};
		const auto score_block = [&](u32 start_pc, u32 instruction_count) {
			for (u32 i = 0; i < instruction_count; i++)
			{
				const u32 op = memRead32(start_pc + i * sizeof(u32));
				switch (op >> 26)
				{
					case 0x11: // COP1
					case 0x12: // COP2
					case 0x31: // LWC1
					case 0x36: // LQC2
					case 0x39: // SWC1
					case 0x3e: // SQC2
						return false;
					default:
						break;
				}

				GprPinOpInfo read_info;
				DirtyGprPinOpInfo write_info;
				if (!ClassifyOpcodeForGprPinning(op, &read_info) ||
					!ClassifyOpcodeForDirtyGprPins(op, &write_info))
				{
					return false;
				}
				for (unsigned read = 0; read < read_info.low_read_count; read++)
					scores[read_info.low_reads[read]] += 2;
				for (unsigned read = 0; read < read_info.dword_read_count; read++)
				{
					scores[read_info.dword_reads[read]] += 2;
					dword_scores[read_info.dword_reads[read]] += 2;
				}
				for (unsigned write = 0; write < write_info.write_count; write++)
				{
					scores[write_info.writes[write]]++;
					writes[write_info.writes[write]]++;
				}
			}
			return true;
		};
		if (!score_block(first_pc, first_instruction_count) ||
			!score_block(second_pc, second_instruction_count))
		{
			return false;
		}

		// r6 is callee-saved by AAPCS and already part of the persistent private
		// frame. Carry PCSX2 iBranchTest()'s signed cycle/deadline delta there only
		// for lowerings whose audited A32 templates never use HOST_TMP5. This first
		// set covers the measured reciprocal texture-transfer loop; every expansion
		// must audit both its hot lowering and any returning cold helper tail.
		const auto block_preserves_scheduler_host = [](u32 start_pc, u32 instruction_count) {
			for (u32 i = 0; i < instruction_count; i++)
			{
				const u32 op = memRead32(start_pc + i * sizeof(u32));
				if (op == 0)
					continue;
				const unsigned opcode = op >> 26;
				if (opcode == 0)
				{
					const unsigned function = op & 0x3fu;
					if (function == 0x2a || function == 0x2b) // SLT/SLTU
						continue;
					return false;
				}
				switch (opcode)
				{
					case 0x04: // BEQ
					case 0x05: // BNE
					case 0x09: // ADDIU
					case 0x14: // BEQL
					case 0x15: // BNEL
						continue;
					case 0x23: // LW
						// A handler-backed counter-page LW in a branch delay slot
						// temporarily saves the branch predicate in r6.
						if (i + 1 == instruction_count)
							return false;
						continue;
					default:
						return false;
				}
			}
			return true;
		};
		const auto block_exits_wait_loop = [](u32 start_pc, u32 instruction_count) {
			if (instruction_count < 2)
				return false;
			const u32 branch_pc = start_pc + (instruction_count - 2) * sizeof(u32);
			const u32 branch_op = memRead32(branch_pc);
			const unsigned opcode = branch_op >> 26;
			if (opcode != 0x04 && opcode != 0x05 && opcode != 0x14 && opcode != 0x15)
				return false;
			const s32 displacement = static_cast<s16>(branch_op & 0xffffu) * 4;
			const u32 target_pc = branch_pc + sizeof(u32) + displacement;
			const u32 end_pc = start_pc + instruction_count * sizeof(u32);
			return target_pc <= start_pc &&
				IsWaitLoopBody(target_pc, end_pc, branch_pc);
		};
		if (block_preserves_scheduler_host(first_pc, first_instruction_count) &&
			block_preserves_scheduler_host(second_pc, second_instruction_count) &&
			!block_exits_wait_loop(first_pc, first_instruction_count) &&
			!block_exits_wait_loop(second_pc, second_instruction_count))
		{
			signature->scheduler.host = GprLinkSignature::SCHEDULER_HOST;
		}

		signature->block_pcs[0] = first_pc < second_pc ? first_pc : second_pc;
		signature->block_pcs[1] = first_pc < second_pc ? second_pc : first_pc;
		const auto allocate_mappings = [&](bool retain_all_dword_values) {
			constexpr u8 default_hosts[] = {9, 10, 11};
			constexpr u8 reclaimed_hosts[] = {7, 8, 9, 10, 11, 14, 1, 3};
			const u8* hosts = retain_all_dword_values ? reclaimed_hosts : default_hosts;
			const size_t host_count = retain_all_dword_values ?
				(sizeof(reclaimed_hosts) / sizeof(reclaimed_hosts[0])) :
				(sizeof(default_hosts) / sizeof(default_hosts[0]));
			u16 remaining_scores[32];
			u16 remaining_dword_scores[32];
			for (unsigned reg = 0; reg < 32; reg++)
			{
				remaining_scores[reg] = scores[reg];
				remaining_dword_scores[reg] = dword_scores[reg];
			}
			signature->count = 0;
			const auto add_mapping = [&](unsigned guest, unsigned low_host,
				unsigned high_host, GprLinkWidth width) {
				if (signature->count >= GprLinkSignature::MAX_PINS)
					return false;
				GprLinkMapping& mapping = signature->mappings[signature->count++];
				mapping.guest = static_cast<u8>(guest);
				mapping.low_host = static_cast<u8>(low_host);
				mapping.high_host = width == GprLinkWidth::Low64 ?
					static_cast<u8>(high_host) : GprLinkMapping::NO_HOST;
				mapping.width = width;
				mapping.dirty = writes[guest] != 0 ?
					GprLinkDirtyState::WriteBack : GprLinkDirtyState::Clean;
				return true;
			};

			size_t next_host = 0;
			while (next_host + 1 < host_count)
			{
				unsigned full_reg = 0;
				u16 full_score = retain_all_dword_values ? 0 : 3;
				for (unsigned reg = 1; reg < 32; reg++)
				{
					if (remaining_dword_scores[reg] > full_score)
					{
						full_reg = reg;
						full_score = remaining_dword_scores[reg];
					}
				}
				if (full_reg == 0 ||
					!add_mapping(full_reg, hosts[next_host], hosts[next_host + 1],
						GprLinkWidth::Low64))
				{
					break;
				}
				next_host += 2;
				remaining_scores[full_reg] = 0;
				remaining_dword_scores[full_reg] = 0;
				if (!retain_all_dword_values)
					break;
			}

			while (next_host < host_count)
			{
				unsigned best_reg = 0;
				u16 best_score = 0;
				for (unsigned reg = 1; reg < 32; reg++)
				{
					if (remaining_scores[reg] > best_score)
					{
						best_reg = reg;
						best_score = remaining_scores[reg];
					}
				}
				if (best_reg == 0 ||
					!add_mapping(best_reg, hosts[next_host], GprLinkMapping::NO_HOST,
						GprLinkWidth::Low32))
				{
					return false;
				}
				next_host++;
				remaining_scores[best_reg] = 0;
				remaining_dword_scores[best_reg] = 0;
			}
			return true;
		};
		if (!allocate_mappings(false))
			return false;

		// PCSX2 owner: recVTLB.cpp::DynGen_PrepRegs() keeps the translated
		// address live through the direct memory operation, while iCore.cpp keeps
		// the induction GPR in its host mapping. Cortex-A9 cannot reserve x86's
		// 4 GiB fastmem window, so recognize the measured reciprocal LW/SLTU/BNEL
		// induction chain and carry its direct vTLB pointer in caller-saved r12.
		// Every accepted lowering is audited to preserve r12; handler calls poison
		// it before rejoining generated code.
		const auto try_add_vtlb_pointer = [&](u32 access_pc, u32 access_count,
			u32 advance_pc, u32 advance_count) {
			if (access_count != 3 || advance_count != 2 ||
				advance_pc != access_pc + 3 * sizeof(u32))
			{
				return false;
			}

			const u32 load = memRead32(access_pc);
			const u32 exit_branch = memRead32(access_pc + sizeof(u32));
			const u32 compare = memRead32(access_pc + 2 * sizeof(u32));
			const u32 loop_branch = memRead32(advance_pc);
			const u32 increment = memRead32(advance_pc + sizeof(u32));
			if ((load >> 26) != 0x23 || IMM_S(load) != 0 ||
				(exit_branch >> 26) != 0x04 ||
				(compare >> 26) != 0 || (compare & 0x3fu) != 0x2b ||
				(loop_branch >> 26) != 0x15 ||
				(increment >> 26) != 0x09)
			{
				return false;
			}

			const unsigned base = RS(load);
			const unsigned result = RT(load);
			const bool exit_reads_result =
				RS(exit_branch) == result || RT(exit_branch) == result;
			const bool loop_reads_result_and_zero =
				(RS(loop_branch) == result && RT(loop_branch) == 0) ||
				(RT(loop_branch) == result && RS(loop_branch) == 0);
			if (base == 0 || result == 0 || !exit_reads_result ||
				RD(compare) != result || RS(compare) != base ||
				!loop_reads_result_and_zero || BranchTarget(advance_pc, loop_branch) != access_pc ||
				RS(increment) != base || RT(increment) != base || IMM_S(increment) != 4)
			{
				return false;
			}

			bool address_is_mapped = false;
			for (u8 i = 0; i < signature->count; i++)
			{
				const GprLinkMapping& mapping = signature->mappings[i];
				if (mapping.guest == base &&
					mapping.dirty == GprLinkDirtyState::WriteBack)
				{
					address_is_mapped = true;
					break;
				}
			}
			if (!address_is_mapped)
				return false;

			signature->vtlb_pointer.host = GprLinkSignature::VTLB_POINTER_HOST;
			signature->vtlb_pointer.guest_address = static_cast<u8>(base);
			signature->vtlb_pointer.guest_result = static_cast<u8>(result);
			signature->vtlb_pointer.stride = 4;
			signature->vtlb_pointer.access_pc = access_pc;
			signature->vtlb_pointer.advance_pc = advance_pc;
			return true;
		};
		if (!try_add_vtlb_pointer(first_pc, first_instruction_count,
				second_pc, second_instruction_count))
		{
			try_add_vtlb_pointer(second_pc, second_instruction_count,
				first_pc, first_instruction_count);
		}
		if (reclaim_vtlb_hosts && signature->vtlb_pointer.IsValid() &&
			!allocate_mappings(true))
		{
			return false;
		}
		if (signature->vtlb_pointer.IsValid())
		{
			bool address_is_mapped = false;
			for (u8 i = 0; i < signature->count; i++)
			{
				address_is_mapped |=
					signature->mappings[i].guest == signature->vtlb_pointer.guest_address &&
					signature->mappings[i].dirty == GprLinkDirtyState::WriteBack;
			}
			if (!address_is_mapped)
				return false;
		}
		return signature->IsValid();
	}

	bool BlockHasExactConditionalSelfLink(u32 start_pc, u32 instruction_count)
	{
		if (instruction_count < 2)
			return false;

		const u32 branch_pc = start_pc + (instruction_count - 2) * sizeof(u32);
		const u32 branch_op = memRead32(branch_pc);
		if (!BlockCompiler::IsSupportedBranchOpcode(branch_op) ||
			IsBranchLikelyOpcode(branch_op))
		{
			return false;
		}

		// Register and absolute jumps do not carry a conditional taken edge.
		// Every remaining supported branch form uses BranchTarget()'s signed
		// PC-relative displacement.
		const unsigned opcode = branch_op >> 26;
		return opcode != 0x00 && opcode != 0x02 && opcode != 0x03 &&
			BranchTarget(branch_pc, branch_op) == start_pc;
	}

	bool BlockCanUseCallerSavedBranchFlag(u32 start_pc, u32 instruction_count)
	{
		if (!BlockHasExactConditionalSelfLink(start_pc, instruction_count))
			return false;

		// The branch predicate is produced before the delay slot. Caller-saved
		// r12 is therefore valid only when that slot cannot call a helper and its
		// native lowering does not use r12. These forms use r0-r2 plus any pinned
		// destination, and the exact self-link tail consumes the predicate before
		// any other caller-saved seam.
		const u32 delay_op = memRead32(start_pc + (instruction_count - 1) * sizeof(u32));
		return delay_op == 0 || (delay_op >> 26) == 0x08 || (delay_op >> 26) == 0x09;
	}

	bool AnalyzeForwardedBooleanBranch(u32 start_pc, u32 instruction_count,
		u8* guest_reg, u32* producer_index)
	{
		if (!guest_reg || !producer_index || instruction_count < 3 ||
			!BlockHasExactConditionalSelfLink(start_pc, instruction_count))
		{
			return false;
		}

		const u32 branch_index = instruction_count - 2;
		const u32 branch_op = memRead32(start_pc + branch_index * sizeof(u32));
		if ((branch_op >> 26) != 0x05) // BNE boolean,zero,loop
			return false;

		const unsigned rs = RS(branch_op);
		const unsigned rt = RT(branch_op);
		const unsigned boolean_guest = (rs == 0) ? rt : ((rt == 0) ? rs : 0);
		if (boolean_guest == 0)
			return false;

		u32 producer = branch_index;
		while (producer != 0 && memRead32(start_pc + (producer - 1) * sizeof(u32)) == 0)
			producer--;
		if (producer == 0)
			return false;
		producer--;

		const u32 producer_op = memRead32(start_pc + producer * sizeof(u32));
		if ((producer_op >> 26) != 0x00 ||
			((producer_op & 0x3f) != 0x2a && (producer_op & 0x3f) != 0x2b) ||
			RD(producer_op) != boolean_guest)
		{
			return false;
		}

		// r12 carries the low boolean and r6 its known-zero high word. Before
		// the producer, accept only NOP and SQ zero: their audited native fast
		// paths preserve both hosts, and SQ's handler tail synchronizes them before
		// calling PCSX2. Also reject SQ using the forwarded guest as its base so a
		// resident iteration never reads stale backing state.
		for (u32 i = 0; i < producer; i++)
		{
			const u32 op = memRead32(start_pc + i * sizeof(u32));
			if (op == 0)
				continue;
			if ((op >> 26) != 0x1f || RT(op) != 0 || RS(op) == boolean_guest)
				return false;
		}

		*guest_reg = static_cast<u8>(boolean_guest);
		*producer_index = producer;
		return true;
	}

	bool ForwardedBooleanPrefixStoresRawGpr0(u32 start_pc, u32 producer_index)
	{
		for (u32 i = 0; i < producer_index; i++)
		{
			const u32 op = memRead32(start_pc + i * sizeof(u32));
			if ((op >> 26) == 0x1f && RT(op) == 0) // SQ zero,imm(rs)
				return true;
		}
		return false;
	}

	bool AnalyzeResidentSequentialRawGpr0QwordStore(u32 start_pc, u32 instruction_count,
		u32 producer_index, u32* store_op)
	{
		if (!store_op || instruction_count < 3)
			return false;

		u32 candidate = 0;
		for (u32 i = 0; i < producer_index; i++)
		{
			const u32 op = memRead32(start_pc + i * sizeof(u32));
			if (op == 0)
				continue;
			if (candidate != 0 || (op >> 26) != 0x1f || RT(op) != 0 || RS(op) == 0)
				return false;
			candidate = op;
		}
		if (candidate == 0)
			return false;

		const u32 delay_op = memRead32(start_pc + (instruction_count - 1) * sizeof(u32));
		if ((delay_op >> 26) != 0x09 || RS(delay_op) != RS(candidate) ||
			RT(delay_op) != RS(candidate) || IMM_S(delay_op) != 16)
		{
			return false;
		}

		*store_op = candidate;
		return true;
	}

	bool UpdateGprPinEntryLiveness(u32 op, u32& defined, u32& live_in_reads)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp scalar ALU/shift/move/mult-div
		// operations, with the matching native paths in EmitSPECIAL()/EmitOpcode().
		// Track only consecutive helper-free operations. Callers stop before any
		// opcode which can call, fault, branch, or otherwise expose the backing GPR
		// file before a pin is initialized.
		const auto read = [&](unsigned guest_reg) {
			const u32 bit = 1u << guest_reg;
			if ((defined & bit) == 0)
				live_in_reads |= bit;
		};
		const auto define = [&](unsigned guest_reg) {
			defined |= 1u << guest_reg;
		};

		const unsigned opcode = op >> 26;
		switch (opcode)
		{
			case 0x00:
				switch (op & 0x3f)
	{
					case 0x00: // SLL
					case 0x02: // SRL
					case 0x03: // SRA
					case 0x38: // DSLL
					case 0x3a: // DSRL
					case 0x3b: // DSRA
					case 0x3c: // DSLL32
					case 0x3e: // DSRL32
					case 0x3f: // DSRA32
						if (RD(op) != 0)
						{
							read(RT(op));
							define(RD(op));
						}
						return true;

					case 0x04: // SLLV
					case 0x06: // SRLV
					case 0x07: // SRAV
					case 0x14: // DSLLV
					case 0x16: // DSRLV
					case 0x17: // DSRAV
						if (RD(op) != 0)
						{
							read(RT(op));
							read(RS(op));
							define(RD(op));
						}
						return true;

					case 0x0a: // MOVZ
					case 0x0b: // MOVN
						if (RD(op) != 0)
						{
							read(RS(op));
							read(RT(op));
							read(RD(op)); // False predicate preserves the old destination.
						}
						return true;

					case 0x0f: // SYNC is a PCSX2 no-op.
						return true;

					case 0x10: // MFHI
					case 0x12: // MFLO
					case 0x28: // MFSA
						define(RD(op));
						return true;

					case 0x11: // MTHI
					case 0x13: // MTLO
					case 0x29: // MTSA
						read(RS(op));
						return true;

					case 0x18: // MULT
					case 0x19: // MULTU
						read(RS(op));
						read(RT(op));
						define(RD(op));
						return true;

					case 0x1a: // DIV
					case 0x1b: // DIVU
						read(RS(op));
						read(RT(op));
						return true;

					case 0x20: // ADD, compiled like ADDU by PCSX2's recompiler.
					case 0x21: // ADDU
					case 0x22: // SUB, compiled like SUBU by PCSX2's recompiler.
					case 0x23: // SUBU
					case 0x24: // AND
					case 0x25: // OR
					case 0x26: // XOR
					case 0x27: // NOR
					case 0x2a: // SLT
					case 0x2b: // SLTU
					case 0x2c: // DADD, compiled like DADDU by PCSX2's recompiler.
					case 0x2d: // DADDU
					case 0x2e: // DSUB, compiled like DSUBU by PCSX2's recompiler.
					case 0x2f: // DSUBU
						if (RD(op) != 0)
						{
							read(RS(op));
							read(RT(op));
							define(RD(op));
						}
						return true;

					default:
						return false;
	}

			case 0x01: // MTSAB / MTSAH share the REGIMM encoding space.
				if (RT(op) == 0x18 || RT(op) == 0x19)
	{
					read(RS(op));
					return true;
	}
				return false;

			case 0x08: // ADDI, compiled like ADDIU by PCSX2's recompiler.
			case 0x09: // ADDIU
			case 0x0a: // SLTI
			case 0x0b: // SLTIU
			case 0x0c: // ANDI
			case 0x0d: // ORI
			case 0x0e: // XORI
			case 0x18: // DADDI, compiled like DADDIU by PCSX2's recompiler.
			case 0x19: // DADDIU
				if (RT(op) != 0)
	{
					read(RS(op));
					define(RT(op));
	}
				return true;

			case 0x0f: // LUI has no GPR source.
				define(RT(op));
				return true;

			case 0x2f: // PCSX2 no-ops IXIN and BFH CACHE forms.
				return IsNoOpCACHE(op);

			case 0x33: // PREF is a PCSX2 no-op.
				return true;

			default:
				return false;
		}
	}

	bool BlockShouldUseCop1ExponentMaskRegister(u32 start_pc, u32 instruction_count)
	{
		unsigned mask_users = 0;
		for (u32 i = 0; i < instruction_count; i++)
		{
			if (FastCOP1UsesExponentMask(memRead32(start_pc + i * 4)) && ++mask_users >= 2)
				return true;
		}

		return false;
	}

	bool BlockShouldUseVu0BaseRegister(u32 start_pc, u32 instruction_count)
	{
		unsigned address_users = 0;
		for (u32 i = 0; i < instruction_count; i++)
		{
			address_users += FastVu0AddressUses(memRead32(start_pc + i * 4));
			if (address_users >= 3)
				return true;
		}

		return false;
	}

	// Pure scalar ops can sit between qword/MMI users without spilling the
	// block-local NEON qcache; their GPR writes still go through the store seams
	// that invalidate stale guest mappings.
	bool ScalarOpcodeKeepsGprQCacheLocal(u32 op)
	{
		switch (op >> 26)
		{
			case 0x00:
				switch (op & 0x3f)
	{
					case 0x00: // SLL
					case 0x02: // SRL
					case 0x03: // SRA
					case 0x04: // SLLV
					case 0x06: // SRLV
					case 0x07: // SRAV
					case 0x08: // JR
					case 0x09: // JALR
					case 0x0a: // MOVZ
					case 0x0b: // MOVN
					case 0x0f: // SYNC
					case 0x14: // DSLLV
					case 0x16: // DSRLV
					case 0x17: // DSRAV
					case 0x20: // ADD
					case 0x21: // ADDU
					case 0x22: // SUB
					case 0x23: // SUBU
					case 0x24: // AND
					case 0x25: // OR
					case 0x26: // XOR
					case 0x27: // NOR
					case 0x2a: // SLT
					case 0x2b: // SLTU
					case 0x2c: // DADD
					case 0x2d: // DADDU
					case 0x2e: // DSUB
					case 0x2f: // DSUBU
					case 0x38: // DSLL
					case 0x3a: // DSRL
					case 0x3b: // DSRA
					case 0x3c: // DSLL32
					case 0x3e: // DSRL32
					case 0x3f: // DSRA32
						return true;
					default:
						return false;
	}
			case 0x01:
				switch (RT(op))
	{
					case 0x00: // BLTZ
					case 0x01: // BGEZ
					case 0x02: // BLTZL
					case 0x03: // BGEZL
					case 0x10: // BLTZAL
					case 0x11: // BGEZAL
					case 0x12: // BLTZALL
					case 0x13: // BGEZALL
					case 0x18: // MTSAB
					case 0x19: // MTSAH
						return true;
					default:
						return false;
	}
			case 0x02: // J
			case 0x03: // JAL
			case 0x04: // BEQ
			case 0x05: // BNE
			case 0x06: // BLEZ
			case 0x07: // BGTZ
			case 0x08: // ADDI
			case 0x09: // ADDIU
			case 0x0a: // SLTI
			case 0x0b: // SLTIU
			case 0x0c: // ANDI
			case 0x0d: // ORI
			case 0x0e: // XORI
			case 0x0f: // LUI
			case 0x14: // BEQL
			case 0x15: // BNEL
			case 0x16: // BLEZL
			case 0x17: // BGTZL
			case 0x18: // DADDI
			case 0x19: // DADDIU
				return true;
			default:
				return false;
		}
	}

	bool VectorOpcodeKeepsGprQCacheLocal(u32 op)
	{
		switch (op >> 26)
		{
			case 0x1e: // LQ, owned by R5900OpcodeImpl.cpp::LQ().
			case 0x1f: // SQ, owned by R5900OpcodeImpl.cpp::SQ().
			case 0x28: // SB, owned by R5900OpcodeImpl.cpp::SB().
			case 0x29: // SH, owned by R5900OpcodeImpl.cpp::SH().
			case 0x2a: // SWL, owned by R5900OpcodeImpl.cpp::SWL().
			case 0x2b: // SW, owned by R5900OpcodeImpl.cpp::SW().
			case 0x2e: // SWR, owned by R5900OpcodeImpl.cpp::SWR().
			case 0x3f: // SD, owned by R5900OpcodeImpl.cpp::SD().
				return true;

			case 0x20: // LB, owned by R5900OpcodeImpl.cpp::LB().
			case 0x21: // LH, owned by R5900OpcodeImpl.cpp::LH().
			case 0x23: // LW, owned by R5900OpcodeImpl.cpp::LW().
			case 0x24: // LBU, owned by R5900OpcodeImpl.cpp::LBU().
			case 0x25: // LHU, owned by R5900OpcodeImpl.cpp::LHU().
			case 0x27: // LWU, owned by R5900OpcodeImpl.cpp::LWU().
			case 0x37: // LD, owned by R5900OpcodeImpl.cpp::LD().
				// Native scalar loads write RT through the same GPR store seams
				// used by scalar ALU ops, invalidating only that guest mapping.
				// Their cold vTLB tails sync state before helper dispatch.
				return true;
			default:
				break;
		}

		if (IsFastCOP2VectorTransfer(op))
		{
			// PCSX2 owner: VU0.cpp::QMFC2()/QMTC2(). These native paths either
			// read qcached GPR data into VU0 or write GPRs through the qcache-aware
			// store seam, so they do not force a block-local qcache spill.
			return true;
		}
		if (IsFastCOP2ControlRead(op))
		{
			// PCSX2 owner: VU0.cpp::CFC2(). Native control reads update GPRs
			// through scalar store seams, so they only invalidate the written
			// guest and do not clobber resident qregs.
			return true;
		}
		if (IsFastCOP2ControlWrite(op))
		{
			// PCSX2 owner: VU0.cpp::CTC2(). Active native VI writes read the
			// low GPR word through the qcache-aware raw-zero load seam. Keep
			// reset and CMSAR1 out because they call helpers or exit at an event.
			const unsigned fs = RD(op);
			return fs != VU0_REG_FBRST && fs != VU0_REG_CMSAR1;
		}

		if ((op >> 26) != 0x1c)
			return false;

		switch (op & 0x3f)
		{
			case 0x00: // MADD
			case 0x01: // MADDU
			case 0x04: // PLZCW
			case 0x10: // MFHI1
			case 0x11: // MTHI1
			case 0x12: // MFLO1
			case 0x13: // MTLO1
			case 0x18: // MULT1
			case 0x19: // MULTU1
			case 0x20: // MADD1
			case 0x21: // MADDU1
			case 0x30: // PMFHL
			case 0x31: // PMTHL
			case 0x34: // PSLLH
			case 0x36: // PSRLH
			case 0x37: // PSRAH
			case 0x3c: // PSLLW
			case 0x3e: // PSRLW
			case 0x3f: // PSRAW
				return CanCompileMMI(op);
			case 0x08: // MMI0 class; every accepted op is pure A32/NEON here.
				return CanCompileMMI0(op);
			case 0x28: // MMI1 class; every accepted op is pure A32/NEON here.
				return CanCompileMMI1(op);
			case 0x09: // MMI2 class; accepted ops are pure A32/NEON here.
				return CanCompileMMI2(op);
			case 0x29: // MMI3 class; accepted ops are pure A32/NEON here.
				return CanCompileMMI3(op);
			default:
				return false;
		}
	}

	bool BlockShouldUseGprQCache(u32 start_pc, u32 instruction_count)
	{
		if (instruction_count == 0)
			return false;

		bool has_qcache_user = false;
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 op = memRead32(start_pc + i * 4);
			if (VectorOpcodeKeepsGprQCacheLocal(op))
			{
				has_qcache_user = true;
				continue;
			}

			if (!ScalarOpcodeKeepsGprQCacheLocal(op))
				return false;
		}

		return has_qcache_user;
	}

	void CountGprQCacheEntryQwordUses(u32 op, u16 counts[32], u32& defined)
	{
		const auto add_read = [&](unsigned guest_reg) {
			if (guest_reg != 0 && (defined & (1u << guest_reg)) == 0)
				counts[guest_reg]++;
		};
		const auto define = [&](unsigned guest_reg) {
			if (guest_reg != 0)
				defined |= 1u << guest_reg;
		};

		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		switch (op >> 26)
		{
			case 0x00:
				switch (op & 0x3f)
	{
					case 0x00: // SLL
					case 0x02: // SRL
					case 0x03: // SRA
					case 0x04: // SLLV
					case 0x06: // SRLV
					case 0x07: // SRAV
					case 0x0a: // MOVZ
					case 0x0b: // MOVN
					case 0x20: // ADD
					case 0x21: // ADDU
					case 0x22: // SUB
					case 0x23: // SUBU
					case 0x24: // AND
					case 0x25: // OR
					case 0x26: // XOR
					case 0x27: // NOR
					case 0x2a: // SLT
					case 0x2b: // SLTU
					case 0x2c: // DADD
					case 0x2d: // DADDU
					case 0x2e: // DSUB
					case 0x2f: // DSUBU
					case 0x38: // DSLL
					case 0x3a: // DSRL
					case 0x3b: // DSRA
					case 0x3c: // DSLL32
					case 0x3e: // DSRL32
					case 0x3f: // DSRA32
						define(rd);
						break;
					default:
						break;
	}
				return;
			case 0x08: // ADDI
			case 0x09: // ADDIU
			case 0x0a: // SLTI
			case 0x0b: // SLTIU
			case 0x0c: // ANDI
			case 0x0d: // ORI
			case 0x0e: // XORI
			case 0x0f: // LUI
			case 0x18: // DADDI
			case 0x19: // DADDIU
			case 0x20: // LB
			case 0x21: // LH
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
			case 0x27: // LWU
			case 0x37: // LD
				define(rt);
				return;
			case 0x1e: // LQ
				define(rt);
				return;
			case 0x1f: // SQ
				add_read(rt);
				return;
			case 0x1c:
				break;
			default:
				return;
		}

		switch (op & 0x3f)
		{
			case 0x34: // PSLLH, owned by MMI.cpp::PSLLH().
			case 0x36: // PSRLH, owned by MMI.cpp::PSRLH().
			case 0x37: // PSRAH, owned by MMI.cpp::PSRAH().
			case 0x3c: // PSLLW, owned by MMI.cpp::PSLLW().
			case 0x3e: // PSRLW, owned by MMI.cpp::PSRLW().
			case 0x3f: // PSRAW, owned by MMI.cpp::PSRAW().
				add_read(rt);
				define(rd);
				return;
			case 0x08: // MMI0 vector/shuffle forms.
			{
				const unsigned sub = (op >> 6) & 0x1f;
				switch (sub)
	{
					case 0x00: // PADDW, owned by MMI.cpp::PADDW().
					case 0x01: // PSUBW, owned by MMI.cpp::PSUBW().
					case 0x02: // PCGTW, owned by MMI.cpp::PCGTW().
					case 0x03: // PMAXW, owned by MMI.cpp::PMAXW().
					case 0x04: // PADDH, owned by MMI.cpp::PADDH().
					case 0x05: // PSUBH, owned by MMI.cpp::PSUBH().
					case 0x06: // PCGTH, owned by MMI.cpp::PCGTH().
					case 0x07: // PMAXH, owned by MMI.cpp::PMAXH().
					case 0x08: // PADDB, owned by MMI.cpp::PADDB().
					case 0x09: // PSUBB, owned by MMI.cpp::PSUBB().
					case 0x0a: // PCGTB, owned by MMI.cpp::PCGTB().
					case 0x10: // PADDSW, owned by MMI.cpp::PADDSW().
					case 0x11: // PSUBSW, owned by MMI.cpp::PSUBSW().
					case 0x12: // PEXTLW, owned by MMI.cpp::PEXTLW().
					case 0x13: // PPACW, owned by MMI.cpp::PPACW().
					case 0x14: // PADDSH, owned by MMI.cpp::PADDSH().
					case 0x15: // PSUBSH, owned by MMI.cpp::PSUBSH().
					case 0x16: // PEXTLH, owned by MMI.cpp::PEXTLH().
					case 0x17: // PPACH, owned by MMI.cpp::PPACH().
					case 0x18: // PADDSB, owned by MMI.cpp::PADDSB().
					case 0x19: // PSUBSB, owned by MMI.cpp::PSUBSB().
					case 0x1a: // PEXTLB, owned by MMI.cpp::PEXTLB().
					case 0x1b: // PPACB, owned by MMI.cpp::PPACB().
						add_read(rs);
						add_read(rt);
						define(rd);
						break;
					case 0x1e: // PEXT5, owned by MMI.cpp::PEXT5().
					case 0x1f: // PPAC5, owned by MMI.cpp::PPAC5().
						add_read(rt);
						define(rd);
						break;
					default:
						break;
	}
				return;
			}
			case 0x28: // MMI1 vector/shuffle forms.
			{
				const unsigned sub = (op >> 6) & 0x1f;
				switch (sub)
	{
					case 0x01: // PABSW, owned by MMI.cpp::PABSW().
					case 0x05: // PABSH, owned by MMI.cpp::PABSH().
						add_read(rt);
						define(rd);
						break;
					case 0x02: // PCEQW, owned by MMI.cpp::PCEQW().
					case 0x03: // PMINW, owned by MMI.cpp::PMINW().
					case 0x04: // PADSBH, owned by MMI.cpp::PADSBH().
					case 0x06: // PCEQH, owned by MMI.cpp::PCEQH().
					case 0x07: // PMINH, owned by MMI.cpp::PMINH().
					case 0x0a: // PCEQB, owned by MMI.cpp::PCEQB().
					case 0x10: // PADDUW, owned by MMI.cpp::PADDUW().
					case 0x11: // PSUBUW, owned by MMI.cpp::PSUBUW().
					case 0x12: // PEXTUW, owned by MMI.cpp::PEXTUW().
					case 0x14: // PADDUH, owned by MMI.cpp::PADDUH().
					case 0x15: // PSUBUH, owned by MMI.cpp::PSUBUH().
					case 0x16: // PEXTUH, owned by MMI.cpp::PEXTUH().
					case 0x18: // PADDUB, owned by MMI.cpp::PADDUB().
					case 0x19: // PSUBUB, owned by MMI.cpp::PSUBUB().
					case 0x1a: // PEXTUB, owned by MMI.cpp::PEXTUB().
						add_read(rs);
						add_read(rt);
						define(rd);
						break;
					default:
						break;
	}
				return;
			}
			case 0x09: // MMI2 vector/multiply/divide forms.
			{
				const unsigned sub = (op >> 6) & 0x1f;
				switch (sub)
	{
					case 0x00: // PMADDW, owned by MMI.cpp::PMADDW().
					case 0x02: // PSLLVW, owned by MMI.cpp::PSLLVW().
					case 0x03: // PSRLVW, owned by MMI.cpp::PSRLVW().
					case 0x04: // PMSUBW, owned by MMI.cpp::PMSUBW().
					case 0x0a: // PINTH, owned by MMI.cpp::PINTH().
					case 0x0c: // PMULTW, owned by MMI.cpp::PMULTW().
					case 0x0e: // PCPYLD, owned by MMI.cpp::PCPYLD().
					case 0x10: // PMADDH, owned by MMI.cpp::PMADDH().
					case 0x11: // PHMADH, owned by MMI.cpp::PHMADH().
					case 0x12: // PAND, owned by MMI.cpp::PAND().
					case 0x13: // PXOR, owned by MMI.cpp::PXOR().
					case 0x14: // PMSUBH, owned by MMI.cpp::PMSUBH().
					case 0x15: // PHMSBH, owned by MMI.cpp::PHMSBH().
					case 0x1c: // PMULTH, owned by MMI.cpp::PMULTH().
						add_read(rs);
						add_read(rt);
						define(rd);
						break;
					case 0x0d: // PDIVW, owned by MMI.cpp::PDIVW().
						add_read(rs);
						add_read(rt);
						break;
					case 0x1d: // PDIVBW, owned by MMI.cpp::PDIVBW().
						// PDIVBW reads all four RS words but only RT.SS[0]; do not
						// stage or preserve RT as a full-qword qcache consumer.
						add_read(rs);
						break;
					case 0x08: // PMFHI, owned by MMI.cpp::PMFHI().
					case 0x09: // PMFLO, owned by MMI.cpp::PMFLO().
						define(rd);
						break;
					case 0x1a: // PEXEH, owned by MMI.cpp::PEXEH().
					case 0x1b: // PREVH, owned by MMI.cpp::PREVH().
					case 0x1e: // PEXEW, owned by MMI.cpp::PEXEW().
					case 0x1f: // PROT3W, owned by MMI.cpp::PROT3W().
						add_read(rt);
						define(rd);
						break;
					default:
						break;
	}
				return;
			}
			case 0x29: // MMI3 vector/multiply/divide forms.
			{
				const unsigned sub = (op >> 6) & 0x1f;
				switch (sub)
	{
					case 0x00: // PMADDUW, owned by MMI.cpp::PMADDUW().
					case 0x03: // PSRAVW, owned by MMI.cpp::PSRAVW().
					case 0x0a: // PINTEH, owned by MMI.cpp::PINTEH().
					case 0x0c: // PMULTUW, owned by MMI.cpp::PMULTUW().
					case 0x0e: // PCPYUD, owned by MMI.cpp::PCPYUD().
					case 0x12: // POR, owned by MMI.cpp::POR().
					case 0x13: // PNOR, owned by MMI.cpp::PNOR().
						add_read(rs);
						add_read(rt);
						define(rd);
						break;
					case 0x0d: // PDIVUW, owned by MMI.cpp::PDIVUW().
						add_read(rs);
						add_read(rt);
						break;
					case 0x08: // PMTHI, owned by MMI.cpp::PMTHI().
					case 0x09: // PMTLO, owned by MMI.cpp::PMTLO().
						add_read(rs);
						break;
					case 0x1a: // PEXCH, owned by MMI.cpp::PEXCH().
					case 0x1b: // PCPYH, owned by MMI.cpp::PCPYH().
					case 0x1e: // PEXCW, owned by MMI.cpp::PEXCW().
						add_read(rt);
						define(rd);
						break;
					default:
						break;
	}
	return;
			}
			default:
				return;
		}
	}

#if defined(VITASX2_QEMU_VALIDATION)
	void CountGprQCacheMissForOpcode(u32 op, unsigned guest_reg,
		bool guest_defined_before_current_instruction,
		u16 guest_entry_qword_read_count)
	{
		const u32 primary = op >> 26;
		if (primary == 0x12)
		{
			g_qemuGprQCacheMissCop2++;
			return;
		}

		if (primary == 0x1e || primary == 0x1f)
		{
			g_qemuGprQCacheMissQwordMemory++;
			return;
		}

		if (primary != 0x1c)
		{
			g_qemuGprQCacheMissOther++;
			return;
		}

		const u32 funct = op & 0x3f;
		const u32 sub = (op >> 6) & 0x1f;
		const auto count_entry_detail = [&](u32& entry, u32& defined,
			u32& read_once, u32& read_twice, u32& read_three_plus) {
			if (guest_defined_before_current_instruction)
			{
				defined++;
			}
			else
			{
				entry++;
				if (guest_entry_qword_read_count <= 1)
					read_once++;
				else if (guest_entry_qword_read_count == 2)
					read_twice++;
				else
					read_three_plus++;
			}
		};
		if ((funct == 0x09 &&
				(sub == 0x00 || sub == 0x04 || sub == 0x0c || sub == 0x0d ||
					sub == 0x10 || sub == 0x11 || sub == 0x14 || sub == 0x15 ||
					sub == 0x1c || sub == 0x1d)) ||
			(funct == 0x29 && (sub == 0x00 || sub == 0x0c || sub == 0x0d)))
		{
			g_qemuGprQCacheMissMmiMultDiv++;
			if (funct == 0x09 && sub == 0x0c)
				g_qemuGprQCacheMissMmiMultDivWordMultiply++;
			else if (funct == 0x29 && sub == 0x0c)
				g_qemuGprQCacheMissMmiMultDivWordMultiply++;
			else if ((funct == 0x09 && (sub == 0x00 || sub == 0x04)) ||
					 (funct == 0x29 && sub == 0x00))
				g_qemuGprQCacheMissMmiMultDivWordMultiplyAdd++;
			else if (funct == 0x09 && sub == 0x1c)
				g_qemuGprQCacheMissMmiMultDivHalfwordMultiply++;
			else if (funct == 0x09 && (sub == 0x10 || sub == 0x14))
				g_qemuGprQCacheMissMmiMultDivHalfwordAccumulate++;
			else if (funct == 0x09 && (sub == 0x11 || sub == 0x15))
				g_qemuGprQCacheMissMmiMultDivHalfwordPairMultiply++;
			else if ((funct == 0x09 && sub == 0x0d) ||
					 (funct == 0x29 && sub == 0x0d))
			{
				g_qemuGprQCacheMissMmiMultDivWordDivide++;
				if (guest_reg == RS(op))
					g_qemuGprQCacheMissMmiMultDivWordDivideRs++;
				if (guest_reg == RT(op))
					g_qemuGprQCacheMissMmiMultDivWordDivideRt++;
				count_entry_detail(g_qemuGprQCacheMissMmiMultDivWordDivideEntry,
					g_qemuGprQCacheMissMmiMultDivWordDivideDefined,
					g_qemuGprQCacheMissMmiMultDivWordDivideEntryReadOnce,
					g_qemuGprQCacheMissMmiMultDivWordDivideEntryReadTwice,
					g_qemuGprQCacheMissMmiMultDivWordDivideEntryReadThreePlus);
			}
			else if (funct == 0x09 && sub == 0x1d)
			{
				g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivide++;
				if (guest_reg == RS(op))
					g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivideRs++;
				if (guest_reg == RT(op))
					g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivideRt++;
				count_entry_detail(g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivideEntry,
					g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivideDefined,
					g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivideEntryReadOnce,
					g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivideEntryReadTwice,
					g_qemuGprQCacheMissMmiMultDivWordByHalfwordDivideEntryReadThreePlus);
			}
			else
				g_qemuGprQCacheMissMmiMultDivOther++;
			return;
		}

		if (funct == 0x10 || funct == 0x20 || funct == 0x30)
		{
			g_qemuGprQCacheMissMmiHiLo++;
			return;
		}

		const bool is_interleave =
			(funct == 0x08 || funct == 0x28) &&
			(sub == 0x12 || sub == 0x16 || sub == 0x1a);
		const bool is_pack_even =
			funct == 0x08 && (sub == 0x13 || sub == 0x17 || sub == 0x1b);
		const bool is_five_bit =
			funct == 0x08 && (sub == 0x1e || sub == 0x1f);
		const bool is_halfword_shuffle =
			(funct == 0x09 || funct == 0x29) &&
			(sub == 0x0a || sub == 0x1a || sub == 0x1b);
		const bool is_word_shuffle =
			(funct == 0x09 && (sub == 0x1e || sub == 0x1f)) ||
			(funct == 0x29 && sub == 0x1e);
		const bool is_dword_pair =
			(funct == 0x09 || funct == 0x29) && sub == 0x0e;
		const bool is_qfsrv = funct == 0x28 && sub == 0x1b;

		if (is_interleave || is_pack_even || is_five_bit ||
			is_halfword_shuffle || is_word_shuffle || is_dword_pair ||
			is_qfsrv)
		{
			g_qemuGprQCacheMissMmiShuffle++;
			if (is_interleave)
			{
				g_qemuGprQCacheMissMmiInterleave++;
			}
				else if (is_pack_even)
				{
					g_qemuGprQCacheMissMmiPackEven++;
					if (guest_reg == RS(op))
						g_qemuGprQCacheMissMmiPackEvenRs++;
					if (guest_reg == RT(op))
						g_qemuGprQCacheMissMmiPackEvenRt++;
					if (guest_defined_before_current_instruction)
					{
						g_qemuGprQCacheMissMmiPackEvenDefined++;
					}
					else
					{
						g_qemuGprQCacheMissMmiPackEvenEntry++;
						if (guest_entry_qword_read_count <= 1)
							g_qemuGprQCacheMissMmiPackEvenEntryReadOnce++;
						else if (guest_entry_qword_read_count == 2)
							g_qemuGprQCacheMissMmiPackEvenEntryReadTwice++;
						else
							g_qemuGprQCacheMissMmiPackEvenEntryReadThreePlus++;
					}
				}
				else if (is_five_bit)
			{
				g_qemuGprQCacheMissMmiFiveBit++;
			}
			else if (is_halfword_shuffle)
			{
				g_qemuGprQCacheMissMmiHalfwordShuffle++;
				if (funct == 0x09 && sub == 0x0a)
					g_qemuGprQCacheMissMmiHalfwordShufflePinth++;
				else if (funct == 0x09 && sub == 0x1a)
					g_qemuGprQCacheMissMmiHalfwordShufflePexeh++;
				else if (funct == 0x09 && sub == 0x1b)
					g_qemuGprQCacheMissMmiHalfwordShufflePrevh++;
				else if (funct == 0x29 && sub == 0x0a)
					g_qemuGprQCacheMissMmiHalfwordShufflePinteh++;
				else if (funct == 0x29 && sub == 0x1a)
					g_qemuGprQCacheMissMmiHalfwordShufflePexch++;
					else if (funct == 0x29 && sub == 0x1b)
						g_qemuGprQCacheMissMmiHalfwordShufflePcpyh++;
					else
						g_qemuGprQCacheMissMmiHalfwordShuffleOtherOp++;
					if (guest_reg == RS(op))
						g_qemuGprQCacheMissMmiHalfwordShuffleRs++;
					if (guest_reg == RT(op))
						g_qemuGprQCacheMissMmiHalfwordShuffleRt++;
					if (guest_defined_before_current_instruction)
					{
						g_qemuGprQCacheMissMmiHalfwordShuffleDefined++;
					}
					else
					{
						g_qemuGprQCacheMissMmiHalfwordShuffleEntry++;
						if (guest_entry_qword_read_count <= 1)
							g_qemuGprQCacheMissMmiHalfwordShuffleEntryReadOnce++;
						else if (guest_entry_qword_read_count == 2)
							g_qemuGprQCacheMissMmiHalfwordShuffleEntryReadTwice++;
						else
							g_qemuGprQCacheMissMmiHalfwordShuffleEntryReadThreePlus++;
					}
				}
			else if (is_word_shuffle)
			{
				g_qemuGprQCacheMissMmiWordShuffle++;
			}
			else if (is_dword_pair)
			{
				g_qemuGprQCacheMissMmiDwordPair++;
			}
			else if (is_qfsrv)
			{
				g_qemuGprQCacheMissMmiQfsrv++;
			}
			else
			{
				g_qemuGprQCacheMissMmiShuffleOther++;
			}
			return;
		}

		g_qemuGprQCacheMissMmiVector++;
		const bool is_wrap_vector =
			funct == 0x08 && (sub == 0x00 || sub == 0x01 || sub == 0x04 ||
								 sub == 0x05 || sub == 0x08 || sub == 0x09);
		const bool is_compare_vector =
			(funct == 0x08 && (sub == 0x02 || sub == 0x03 || sub == 0x06 ||
								  sub == 0x07 || sub == 0x0a)) ||
			(funct == 0x28 && (sub == 0x02 || sub == 0x03 || sub == 0x06 ||
								  sub == 0x07 || sub == 0x0a));
		const bool is_signed_saturating_vector =
			funct == 0x08 && (sub == 0x10 || sub == 0x11 || sub == 0x14 ||
								 sub == 0x15 || sub == 0x18 || sub == 0x19);
		const bool is_unsigned_saturating_vector =
			funct == 0x28 && (sub == 0x10 || sub == 0x11 || sub == 0x14 ||
								 sub == 0x15 || sub == 0x18 || sub == 0x19);
		const bool is_logical_vector =
			(funct == 0x09 || funct == 0x29) && (sub == 0x12 || sub == 0x13);
		const bool is_unary_vector =
			(funct == 0x04 && sub == 0x00) ||
			(funct == 0x28 && (sub == 0x01 || sub == 0x04 || sub == 0x05));
		const bool is_immediate_shift_vector =
			funct == 0x34 || funct == 0x36 || funct == 0x37 ||
			funct == 0x3c || funct == 0x3e || funct == 0x3f;
		const bool is_variable_shift_vector =
			(funct == 0x09 && (sub == 0x02 || sub == 0x03)) ||
			(funct == 0x29 && sub == 0x03);

		if (is_wrap_vector)
			g_qemuGprQCacheMissMmiVectorWrap++;
		else if (is_compare_vector)
			g_qemuGprQCacheMissMmiVectorCompare++;
		else if (is_signed_saturating_vector)
			g_qemuGprQCacheMissMmiVectorSignedSaturating++;
		else if (is_unsigned_saturating_vector)
			g_qemuGprQCacheMissMmiVectorUnsignedSaturating++;
		else if (is_logical_vector)
		{
			g_qemuGprQCacheMissMmiVectorLogical++;
			if (funct == 0x09 && sub == 0x12)
				g_qemuGprQCacheMissMmiVectorLogicalPand++;
			else if (funct == 0x09 && sub == 0x13)
				g_qemuGprQCacheMissMmiVectorLogicalPxor++;
			else if (funct == 0x29 && sub == 0x12)
			{
				g_qemuGprQCacheMissMmiVectorLogicalPor++;
				if (guest_reg == RS(op))
					g_qemuGprQCacheMissMmiVectorLogicalPorRs++;
				if (guest_reg == RT(op))
					g_qemuGprQCacheMissMmiVectorLogicalPorRt++;
				if (guest_defined_before_current_instruction)
				{
					g_qemuGprQCacheMissMmiVectorLogicalPorDefined++;
				}
				else
				{
					g_qemuGprQCacheMissMmiVectorLogicalPorEntry++;
					if (guest_entry_qword_read_count <= 1)
						g_qemuGprQCacheMissMmiVectorLogicalPorEntryReadOnce++;
					else if (guest_entry_qword_read_count == 2)
						g_qemuGprQCacheMissMmiVectorLogicalPorEntryReadTwice++;
					else
						g_qemuGprQCacheMissMmiVectorLogicalPorEntryReadThreePlus++;
				}
			}
			else if (funct == 0x29 && sub == 0x13)
				g_qemuGprQCacheMissMmiVectorLogicalPnor++;
		}
		else if (is_unary_vector)
		{
			g_qemuGprQCacheMissMmiVectorUnary++;
			if (guest_reg == RT(op))
				g_qemuGprQCacheMissMmiVectorUnaryRt++;
			if (guest_defined_before_current_instruction)
			{
				g_qemuGprQCacheMissMmiVectorUnaryDefined++;
			}
			else
			{
				g_qemuGprQCacheMissMmiVectorUnaryEntry++;
				if (guest_entry_qword_read_count <= 1)
					g_qemuGprQCacheMissMmiVectorUnaryEntryReadOnce++;
				else if (guest_entry_qword_read_count == 2)
					g_qemuGprQCacheMissMmiVectorUnaryEntryReadTwice++;
				else
					g_qemuGprQCacheMissMmiVectorUnaryEntryReadThreePlus++;
			}
		}
		else if (is_immediate_shift_vector)
		{
			g_qemuGprQCacheMissMmiVectorImmediateShift++;
			if (funct == 0x34)
				g_qemuGprQCacheMissMmiVectorImmediateShiftPsllh++;
			else if (funct == 0x36)
				g_qemuGprQCacheMissMmiVectorImmediateShiftPsrlh++;
			else if (funct == 0x37)
				g_qemuGprQCacheMissMmiVectorImmediateShiftPsrah++;
			else if (funct == 0x3c)
				g_qemuGprQCacheMissMmiVectorImmediateShiftPsllw++;
			else if (funct == 0x3e)
				g_qemuGprQCacheMissMmiVectorImmediateShiftPsrlw++;
				else if (funct == 0x3f)
					g_qemuGprQCacheMissMmiVectorImmediateShiftPsraw++;
				else
					g_qemuGprQCacheMissMmiVectorImmediateShiftOtherOp++;
				if (guest_reg == RT(op))
					g_qemuGprQCacheMissMmiVectorImmediateShiftRt++;
				if (guest_defined_before_current_instruction)
				{
					g_qemuGprQCacheMissMmiVectorImmediateShiftDefined++;
				}
				else
				{
					g_qemuGprQCacheMissMmiVectorImmediateShiftEntry++;
					if (guest_entry_qword_read_count <= 1)
						g_qemuGprQCacheMissMmiVectorImmediateShiftEntryReadOnce++;
					else if (guest_entry_qword_read_count == 2)
						g_qemuGprQCacheMissMmiVectorImmediateShiftEntryReadTwice++;
					else
						g_qemuGprQCacheMissMmiVectorImmediateShiftEntryReadThreePlus++;
				}
			}
			else if (is_variable_shift_vector)
			{
				g_qemuGprQCacheMissMmiVectorVariableShift++;
				if (guest_reg == RS(op))
					g_qemuGprQCacheMissMmiVectorVariableShiftRs++;
				if (guest_reg == RT(op))
					g_qemuGprQCacheMissMmiVectorVariableShiftRt++;
				if (guest_defined_before_current_instruction)
				{
					g_qemuGprQCacheMissMmiVectorVariableShiftDefined++;
				}
				else
				{
					g_qemuGprQCacheMissMmiVectorVariableShiftEntry++;
					if (guest_entry_qword_read_count <= 1)
						g_qemuGprQCacheMissMmiVectorVariableShiftEntryReadOnce++;
					else if (guest_entry_qword_read_count == 2)
						g_qemuGprQCacheMissMmiVectorVariableShiftEntryReadTwice++;
					else
						g_qemuGprQCacheMissMmiVectorVariableShiftEntryReadThreePlus++;
				}
			}
		else
			g_qemuGprQCacheMissMmiVectorOther++;
	}
#endif

	void BlockCompiler::ClearGprConstState()
	{
		for (unsigned i = 0; i < 32; i++)
		{
			m_gpr_const_known[i] = false;
			m_gpr_const_low[i] = 0;
			m_gpr_const_high_known[i] = false;
			m_gpr_const_high[i] = 0;
		}
		m_gpr_const_known[0] = true;
		m_gpr_const_high_known[0] = true;
	}

	void BlockCompiler::ClearSaConstState()
	{
		m_sa_const_known = false;
		m_sa_const_byte_offset = 0;
	}

	void BlockCompiler::ClearCop1NormalizedState()
	{
		for (unsigned i = 0; i < 32; i++)
			m_cop1_fpr_normalized[i] = false;
		m_cop1_acc_normalized = false;
	}

	bool BlockCompiler::IsCop1FprNormalized(unsigned fpr) const
	{
		return fpr < 32 && m_cop1_fpr_normalized[fpr];
	}

	bool BlockCompiler::IsCop1AccNormalized() const
	{
		return m_cop1_acc_normalized;
	}

	bool BlockCompiler::TryGetKnownGprLow(unsigned guest_reg, u32* value) const
	{
		if (guest_reg >= 32 || !m_gpr_const_known[guest_reg])
			return false;

		if (value)
			*value = m_gpr_const_low[guest_reg];
		return true;
	}

	bool BlockCompiler::TryGetKnownGpr64(unsigned guest_reg, u32* low, u32* high) const
	{
		if (guest_reg >= 32 || !m_gpr_const_known[guest_reg] || !m_gpr_const_high_known[guest_reg])
			return false;

		if (low)
			*low = m_gpr_const_low[guest_reg];
		if (high)
			*high = m_gpr_const_high[guest_reg];
		return true;
	}

	bool BlockCompiler::EmitStoreKnownSignExtended32(unsigned guest_reg, u32 value)
	{
		if (guest_reg == 0)
			return true;

#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprConstResultStores++;
#endif
		if (value == 0)
			return EmitStoreGprZero64(guest_reg);

		unsigned low_reg = HOST_TMP0;
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			low_reg = static_cast<unsigned>(pin_host);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprConstPinnedStoreOperands++;
#endif
		}

		unsigned high_reg = HOST_TMP1;
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0)
		{
			high_reg = static_cast<unsigned>(high_pin_host);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprConstPinnedStoreOperands++;
#endif
		}

		return m_code.EmitMovImm32(low_reg, value) &&
			   m_code.EmitMovRegShiftImm(high_reg, low_reg, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(guest_reg, low_reg, high_reg);
	}

	bool BlockCompiler::EmitStoreKnownZeroExtended32(unsigned guest_reg, u32 value)
	{
		if (guest_reg == 0)
			return true;

#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprConstResultStores++;
#endif
		if (value == 0)
			return EmitStoreGprZero64(guest_reg);

		unsigned low_reg = HOST_TMP0;
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			low_reg = static_cast<unsigned>(pin_host);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprConstPinnedStoreOperands++;
#endif
		}

		unsigned high_reg = HOST_TMP1;
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0)
		{
			high_reg = static_cast<unsigned>(high_pin_host);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprConstPinnedStoreOperands++;
#endif
		}

		return m_code.EmitMovImm32(low_reg, value) &&
			   m_code.EmitMovImm8(high_reg, 0) &&
			   EmitStoreGpr64(guest_reg, low_reg, high_reg);
	}

	bool BlockCompiler::EmitStoreKnown64(unsigned guest_reg, u32 low, u32 high)
	{
		if (guest_reg == 0)
			return true;

		if (high == 0)
			return EmitStoreKnownZeroExtended32(guest_reg, low);
		if (high == (static_cast<s32>(low) < 0 ? 0xffffffffu : 0))
			return EmitStoreKnownSignExtended32(guest_reg, low);

#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprConstResultStores++;
#endif
		unsigned low_reg = HOST_TMP0;
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			low_reg = static_cast<unsigned>(pin_host);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprConstPinnedStoreOperands++;
#endif
		}

		unsigned high_reg = HOST_TMP1;
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0)
		{
			high_reg = static_cast<unsigned>(high_pin_host);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprConstPinnedStoreOperands++;
#endif
		}

		return m_code.EmitMovImm32(low_reg, low) &&
			   m_code.EmitMovImm32(high_reg, high) &&
			   EmitStoreGpr64(guest_reg, low_reg, high_reg);
	}

	bool BlockCompiler::EmitStoreGprSignExtended32FromLow(unsigned guest_reg, unsigned host_low)
	{
		if (guest_reg == 0)
			return true;

		unsigned low_reg = host_low;
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0 && static_cast<unsigned>(high_pin_host) == low_reg)
		{
			low_reg = (host_low == HOST_TMP0) ? HOST_TMP2 : HOST_TMP0;
			if (!m_code.EmitMovRegShiftImm(low_reg, host_low, VitaA32::ShiftType::LSL, 0))
				return false;
		}

		unsigned high_reg = (low_reg == HOST_TMP1) ? HOST_TMP2 : HOST_TMP1;
		if (high_pin_host >= 0 && static_cast<unsigned>(high_pin_host) != low_reg)
		{
			high_reg = static_cast<unsigned>(high_pin_host);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprPinExtendedHighStoreOperands++;
#endif
		}

		return m_code.EmitMovRegShiftImm(high_reg, low_reg, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(guest_reg, low_reg, high_reg);
	}

	bool BlockCompiler::EmitStoreGprZeroExtended32FromLow(unsigned guest_reg, unsigned host_low)
	{
		if (guest_reg == 0)
			return true;

		unsigned low_reg = host_low;
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0 && static_cast<unsigned>(high_pin_host) == low_reg)
		{
			low_reg = (host_low == HOST_TMP0) ? HOST_TMP2 : HOST_TMP0;
			if (!m_code.EmitMovRegShiftImm(low_reg, host_low, VitaA32::ShiftType::LSL, 0))
				return false;
		}

		unsigned high_reg = (low_reg == HOST_TMP1) ? HOST_TMP2 : HOST_TMP1;
		if (high_pin_host >= 0 && static_cast<unsigned>(high_pin_host) != low_reg)
		{
			high_reg = static_cast<unsigned>(high_pin_host);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprPinExtendedHighStoreOperands++;
#endif
		}

		return m_code.EmitMovImm8(high_reg, 0) &&
			   EmitStoreGpr64(guest_reg, low_reg, high_reg);
	}

	unsigned BlockCompiler::SelectGprLowResultHost(unsigned guest_reg, unsigned fallback_host)
	{
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host < 0)
			return fallback_host;

		// A32 data-processing results may alias either source operand, and other
		// callers use this only after all source operands have been consumed.
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprPinLowResultStoreOperands++;
#endif
		return static_cast<unsigned>(pin_host);
	}

	unsigned BlockCompiler::SelectGprHighResultHost(unsigned guest_reg, unsigned fallback_host)
	{
		const int pin_host = FindGprPinHighHost(guest_reg);
		if (pin_host < 0)
			return fallback_host;

		return static_cast<unsigned>(pin_host);
	}

	bool BlockCompiler::TryGetKnownEffectiveAddress(u32 op, u32* address) const
	{
		const unsigned rs = RS(op);
		const s32 imm = static_cast<s32>(IMM_S(op));
		if (rs == 0)
		{
			*address = static_cast<u32>(imm);
			return true;
		}

		if (!m_gpr_const_known[rs])
			return false;

		*address = m_gpr_const_low[rs] + static_cast<u32>(imm);
		return true;
	}

	bool BlockCompiler::TryEmitKnownVtlbNonHandlerHostAddress(u32 guest_addr, unsigned host_reg,
		KnownVtlbFastPathKind kind)
	{
		if (!vtlb_private::vtlbdata.vmap)
			return false;

		const vtlb_private::VTLBVirtual vmv =
			vtlb_private::vtlbdata.vmap[guest_addr >> vtlb_private::VTLB_PAGE_BITS];
		if (vmv.isHandler(guest_addr))
			return false;

#if defined(VITASX2_QEMU_VALIDATION)
		switch (kind)
		{
			case KnownVtlbFastPathKind::Scalar:
				g_qemuKnownVtlbScalarFastPaths++;
				break;
			case KnownVtlbFastPathKind::Qword:
				g_qemuKnownVtlbQwordFastPaths++;
				break;
			case KnownVtlbFastPathKind::Cop1:
				g_qemuKnownVtlbCop1FastPaths++;
				break;
			case KnownVtlbFastPathKind::Cop2:
				g_qemuKnownVtlbCop2FastPaths++;
				break;
			case KnownVtlbFastPathKind::Partial:
				g_qemuKnownVtlbPartialFastPaths++;
				break;
		}
#endif
		return m_code.EmitMovImm32(host_reg, static_cast<u32>(vmv.assumePtr(guest_addr)));
	}

	bool BlockCompiler::BlockNeedsResidentVtlbRegisters(u32 start_pc, u32 instruction_count)
	{
		bool saw_vtlb_opcode = false;
		ClearGprConstState();

		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 pc = start_pc + i * 4;
			const u32 op = memRead32(pc);
			if (OpcodeMayUseVtlbFastPath(op))
			{
				saw_vtlb_opcode = true;

				u32 known_address = 0;
				if (!TryGetKnownEffectiveAddress(op, &known_address) ||
					!OpcodeUsesKnownVtlbNonHandlerAddress(op, known_address))
	{
					ClearGprConstState();
					return true;
	}
			}

			UpdateGprConstStateAfterOpcode(op, pc);
		}

#if defined(VITASX2_QEMU_VALIDATION)
		if (saw_vtlb_opcode)
			g_qemuKnownVtlbRegisterlessBlocks++;
#endif
		ClearGprConstState();
		return false;
	}

	static bool Cop0OpcodeNeedsLinkedPcSync(u32 op)
	{
		if ((op >> 26) != 0x10)
			return false;

		const unsigned rs = RS(op);
		const unsigned rd = RD(op);
		if (rs == 0x00)
		{
			// x86/iCOP0.cpp::recMFC0() requests FLUSH_INTERPRETER only for
			// live PCR0/PCR1 reads before COP0_UpdatePCCR().
			return rd == 25 && RT(op) != 0 && (op & 1u) != 0;
		}

		if (rs != 0x04)
			return false;

		// x86/iCOP0.cpp::recMTC0() requests FLUSH_INTERPRETER around
		// WriteCP0Status() and the effective MTPS/PCCR helper pair. Other
		// MTC0 forms are inline, and system forms write their own PC before exit.
		return rd == 0x0c ||
			(rd == 0x19 && (op & 1u) == 0 && (op & 0x3eu) == 0);
	}

	static bool BlockNeedsLinkedPcSync(u32 start_pc, u32 instruction_count)
	{
		// Native links may defer the predecessor's backing-PC write. Only blocks
		// which can expose cpuRegs.pc before their own exit need to restore the
		// block-start PC on linked entry. This mirrors PCSX2's iFlushCall(FLUSH_PC)
		// boundary instead of charging every helper-free ALU/MMI/branch block.
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 op = memRead32(start_pc + i * sizeof(u32));
			// vTLB callbacks may inspect backing EE state. COP2's
			// FLUSH_FOR_POSSIBLE_MICRO_EXEC, Goemon's block-start calls, CACHE's
			// executeCacheOp(op, addr), and trace callbacks with an explicit PC do
			// not request or consume the backing PC.
			if (OpcodeMayUseVtlbFastPath(op) || Cop0OpcodeNeedsLinkedPcSync(op))
			{
				return true;
			}
		}

		return false;
	}

	void BlockCompiler::UpdateGprConstStateAfterOpcode(u32 op, u32 pc)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		const auto clear = [this](unsigned guest_reg) {
			if (guest_reg != 0)
			{
				m_gpr_const_known[guest_reg] = false;
				m_gpr_const_high_known[guest_reg] = false;
			}
		};
		const auto set = [this](unsigned guest_reg, u32 value) {
			if (guest_reg != 0)
			{
				m_gpr_const_known[guest_reg] = true;
				m_gpr_const_low[guest_reg] = value;
				m_gpr_const_high_known[guest_reg] = false;
			}
		};
		const auto set64 = [this](unsigned guest_reg, u32 low, u32 high) {
			if (guest_reg != 0)
			{
				m_gpr_const_known[guest_reg] = true;
				m_gpr_const_low[guest_reg] = low;
				m_gpr_const_high_known[guest_reg] = true;
				m_gpr_const_high[guest_reg] = high;
			}
		};
		const auto set_sign32 = [&](unsigned guest_reg, u32 value) {
			set64(guest_reg, value, (static_cast<s32>(value) < 0) ? 0xffffffffu : 0);
		};
		const auto set_zero32 = [&](unsigned guest_reg, u32 value) {
			set64(guest_reg, value, 0);
		};
		const auto known = [this](unsigned guest_reg, u32* value) {
			if (!m_gpr_const_known[guest_reg])
				return false;
			*value = m_gpr_const_low[guest_reg];
			return true;
		};
		const auto known64 = [this](unsigned guest_reg, u32* low, u32* high) {
			if (!m_gpr_const_known[guest_reg] || !m_gpr_const_high_known[guest_reg])
				return false;
			*low = m_gpr_const_low[guest_reg];
			*high = m_gpr_const_high[guest_reg];
			return true;
		};
		const auto set_add64 = [&](unsigned dst, unsigned lhs, u32 rhs_low, u32 rhs_high) {
			u32 lhs_low = 0;
			u32 lhs_high = 0;
			if (!known64(lhs, &lhs_low, &lhs_high))
				return false;

			const u64 result = (static_cast<u64>(lhs_high) << 32) | lhs_low;
			const u64 rhs_value = (static_cast<u64>(rhs_high) << 32) | rhs_low;
			const u64 sum = result + rhs_value;
			set64(dst, static_cast<u32>(sum), static_cast<u32>(sum >> 32));
			return true;
		};
		const auto binary_sign32 = [&](unsigned dst, unsigned lhs, unsigned rhs, u32 (*op_fn)(u32, u32)) {
			u32 lhs_value = 0;
			u32 rhs_value = 0;
			if (known(lhs, &lhs_value) && known(rhs, &rhs_value))
				set_sign32(dst, op_fn(lhs_value, rhs_value));
			else
				clear(dst);
		};
		const auto binary_exact64 = [&](unsigned dst, unsigned lhs, unsigned rhs,
			u64 (*op_fn)(u64, u64), u32 (*low_fn)(u32, u32)) {
			u32 lhs_low = 0;
			u32 rhs_low = 0;
			u32 lhs_high = 0;
			u32 rhs_high = 0;
			if (known64(lhs, &lhs_low, &lhs_high) && known64(rhs, &rhs_low, &rhs_high))
			{
				const u64 lhs_value = (static_cast<u64>(lhs_high) << 32) | lhs_low;
				const u64 rhs_value = (static_cast<u64>(rhs_high) << 32) | rhs_low;
				const u64 result = op_fn(lhs_value, rhs_value);
				set64(dst, static_cast<u32>(result), static_cast<u32>(result >> 32));
			}
			else if (known(lhs, &lhs_low) && known(rhs, &rhs_low))
			{
				set(dst, low_fn(lhs_low, rhs_low));
			}
			else
			{
				clear(dst);
			}
		};
		const auto immediate_low = [&](unsigned dst, unsigned src, u32 imm, u32 (*op_fn)(u32, u32)) {
			u32 src_value = 0;
			if (known(src, &src_value))
				set(dst, op_fn(src_value, imm));
			else
				clear(dst);
		};
		const auto immediate_sign32 = [&](unsigned dst, unsigned src, u32 imm, u32 (*op_fn)(u32, u32)) {
			u32 src_value = 0;
			if (known(src, &src_value))
				set_sign32(dst, op_fn(src_value, imm));
			else
				clear(dst);
		};

		switch (op >> 26)
		{
			case 0x00:
				switch (op & 0x3f)
	{
					case 0x00: // SLL
	{
						u32 value = 0;
						if (known(rt, &value))
							set_sign32(rd, value << SA(op));
						else
							clear(rd);
						return;
	}
					case 0x02: // SRL
	{
						u32 value = 0;
						if (known(rt, &value))
							set_sign32(rd, value >> SA(op));
						else
							clear(rd);
						return;
	}
					case 0x03: // SRA
	{
						u32 value = 0;
						if (known(rt, &value))
							set_sign32(rd, static_cast<u32>(static_cast<s32>(value) >> SA(op)));
						else
							clear(rd);
						return;
	}
					case 0x04: // SLLV
	{
						u32 value = 0;
						u32 amount = 0;
						if (known(rt, &value) && known(rs, &amount))
							set_sign32(rd, value << (amount & 0x1f));
						else
							clear(rd);
						return;
	}
					case 0x06: // SRLV
	{
						u32 value = 0;
						u32 amount = 0;
						if (known(rt, &value) && known(rs, &amount))
							set_sign32(rd, value >> (amount & 0x1f));
						else
							clear(rd);
						return;
	}
					case 0x07: // SRAV
	{
						u32 value = 0;
						u32 amount = 0;
						if (known(rt, &value) && known(rs, &amount))
							set_sign32(rd, static_cast<u32>(static_cast<s32>(value) >> (amount & 0x1f)));
						else
							clear(rd);
						return;
	}
					case 0x20: // ADD
					case 0x21: // ADDU
						binary_sign32(rd, rs, rt, [](u32 lhs, u32 rhs) { return lhs + rhs; });
						return;
					case 0x22: // SUB
					case 0x23: // SUBU
						binary_sign32(rd, rs, rt, [](u32 lhs, u32 rhs) { return lhs - rhs; });
						return;
					case 0x2c: // DADD
					case 0x2d: // DADDU
						binary_exact64(rd, rs, rt,
							[](u64 lhs, u64 rhs) { return lhs + rhs; },
							[](u32 lhs, u32 rhs) { return lhs + rhs; });
						return;
					case 0x2e: // DSUB
					case 0x2f: // DSUBU
						binary_exact64(rd, rs, rt,
							[](u64 lhs, u64 rhs) { return lhs - rhs; },
							[](u32 lhs, u32 rhs) { return lhs - rhs; });
						return;
					case 0x24: // AND
	{
						u32 lhs_low = 0;
						u32 lhs_high = 0;
						u32 rhs_low = 0;
						u32 rhs_high = 0;
						const bool lhs64 = known64(rs, &lhs_low, &lhs_high);
						const bool rhs64 = known64(rt, &rhs_low, &rhs_high);
						if (lhs64 && rhs64)
							set64(rd, lhs_low & rhs_low, lhs_high & rhs_high);
						else if ((lhs64 && lhs_low == 0 && lhs_high == 0) ||
								 (rhs64 && rhs_low == 0 && rhs_high == 0))
							set64(rd, 0, 0);
						else if ((known(rs, &lhs_low) && lhs_low == 0) ||
								 (known(rt, &rhs_low) && rhs_low == 0))
							set(rd, 0);
						else if (known(rs, &lhs_low) && known(rt, &rhs_low))
							set(rd, lhs_low & rhs_low);
						else
							clear(rd);
						return;
	}
					case 0x25: // OR
	{
						u32 lhs_low = 0;
						u32 lhs_high = 0;
						u32 rhs_low = 0;
						u32 rhs_high = 0;
						const bool lhs64 = known64(rs, &lhs_low, &lhs_high);
						const bool rhs64 = known64(rt, &rhs_low, &rhs_high);
						if (lhs64 && rhs64)
							set64(rd, lhs_low | rhs_low, lhs_high | rhs_high);
						else if ((lhs64 && lhs_low == 0xffffffffu && lhs_high == 0xffffffffu) ||
								 (rhs64 && rhs_low == 0xffffffffu && rhs_high == 0xffffffffu))
							set64(rd, 0xffffffffu, 0xffffffffu);
						else if ((known(rs, &lhs_low) && lhs_low == 0xffffffffu) ||
								 (known(rt, &rhs_low) && rhs_low == 0xffffffffu))
							set(rd, 0xffffffffu);
						else if (known(rs, &lhs_low) && known(rt, &rhs_low))
							set(rd, lhs_low | rhs_low);
						else
							clear(rd);
						return;
	}
					case 0x26: // XOR
						binary_exact64(rd, rs, rt,
							[](u64 lhs, u64 rhs) { return lhs ^ rhs; },
							[](u32 lhs, u32 rhs) { return lhs ^ rhs; });
						return;
					case 0x27: // NOR
	{
						u32 lhs_low = 0;
						u32 lhs_high = 0;
						u32 rhs_low = 0;
						u32 rhs_high = 0;
						const bool lhs64 = known64(rs, &lhs_low, &lhs_high);
						const bool rhs64 = known64(rt, &rhs_low, &rhs_high);
						if (lhs64 && rhs64)
							set64(rd, ~(lhs_low | rhs_low), ~(lhs_high | rhs_high));
						else if ((lhs64 && lhs_low == 0xffffffffu && lhs_high == 0xffffffffu) ||
								 (rhs64 && rhs_low == 0xffffffffu && rhs_high == 0xffffffffu))
							set64(rd, 0, 0);
						else if ((known(rs, &lhs_low) && lhs_low == 0xffffffffu) ||
								 (known(rt, &rhs_low) && rhs_low == 0xffffffffu))
							set(rd, 0);
						else if (known(rs, &lhs_low) && known(rt, &rhs_low))
							set(rd, ~(lhs_low | rhs_low));
						else
							clear(rd);
						return;
	}
					case 0x09: // JALR
						set64(rd, pc + 8, 0);
						return;
					case 0x14: // DSLLV
	{
						u32 value_low = 0;
						u32 value_high = 0;
						u32 amount = 0;
						if (known64(rt, &value_low, &value_high) && known(rs, &amount))
						{
							const u64 value = (static_cast<u64>(value_high) << 32) | value_low;
							const u64 result = value << (amount & 0x3f);
							set64(rd, static_cast<u32>(result), static_cast<u32>(result >> 32));
						}
						else
						{
							clear(rd);
						}
						return;
	}
					case 0x16: // DSRLV
	{
						u32 value_low = 0;
						u32 value_high = 0;
						u32 amount = 0;
						if (known64(rt, &value_low, &value_high) && known(rs, &amount))
						{
							const u64 value = (static_cast<u64>(value_high) << 32) | value_low;
							const u64 result = value >> (amount & 0x3f);
							set64(rd, static_cast<u32>(result), static_cast<u32>(result >> 32));
						}
						else
						{
							clear(rd);
						}
						return;
	}
					case 0x17: // DSRAV
	{
						u32 value_low = 0;
						u32 value_high = 0;
						u32 amount = 0;
						if (known64(rt, &value_low, &value_high) && known(rs, &amount))
						{
							const u64 value = (static_cast<u64>(value_high) << 32) | value_low;
							const u64 result = static_cast<u64>(static_cast<s64>(value) >> (amount & 0x3f));
							set64(rd, static_cast<u32>(result), static_cast<u32>(result >> 32));
						}
						else
						{
							clear(rd);
						}
						return;
	}
					case 0x38: // DSLL
					case 0x3c: // DSLL32
	{
						u32 value_low = 0;
						u32 value_high = 0;
						if (known64(rt, &value_low, &value_high))
						{
							const unsigned amount = SA(op) + ((op & 0x3f) == 0x3c ? 32 : 0);
							const u64 value = (static_cast<u64>(value_high) << 32) | value_low;
							const u64 result = value << amount;
							set64(rd, static_cast<u32>(result), static_cast<u32>(result >> 32));
						}
						else
						{
							clear(rd);
						}
						return;
	}
					case 0x3a: // DSRL
					case 0x3e: // DSRL32
	{
						u32 value_low = 0;
						u32 value_high = 0;
						if (known64(rt, &value_low, &value_high))
						{
							const unsigned amount = SA(op) + ((op & 0x3f) == 0x3e ? 32 : 0);
							const u64 value = (static_cast<u64>(value_high) << 32) | value_low;
							const u64 result = value >> amount;
							set64(rd, static_cast<u32>(result), static_cast<u32>(result >> 32));
						}
						else
						{
							clear(rd);
						}
						return;
	}
					case 0x3b: // DSRA
					case 0x3f: // DSRA32
	{
						u32 value_low = 0;
						u32 value_high = 0;
						if (known64(rt, &value_low, &value_high))
						{
							const unsigned amount = SA(op) + ((op & 0x3f) == 0x3f ? 32 : 0);
							const u64 value = (static_cast<u64>(value_high) << 32) | value_low;
							const u64 result = static_cast<u64>(static_cast<s64>(value) >> amount);
							set64(rd, static_cast<u32>(result), static_cast<u32>(result >> 32));
						}
						else
						{
							clear(rd);
						}
						return;
	}
					case 0x0a: // MOVZ
					case 0x0b: // MOVN
	{
						if (rs == rd)
							return;

						u32 condition_low = 0;
						u32 condition_high = 0;
						if (known64(rt, &condition_low, &condition_high))
						{
							const bool condition_zero = condition_low == 0 && condition_high == 0;
							const bool move = (op & 0x3f) == 0x0a ? condition_zero : !condition_zero;
							if (!move)
								return;

							u32 source_low = 0;
							u32 source_high = 0;
							if (known64(rs, &source_low, &source_high))
								set64(rd, source_low, source_high);
							else if (known(rs, &source_low))
								set(rd, source_low);
							else
								clear(rd);
							return;
						}

						clear(rd);
						return;
	}
					case 0x10: // MFHI
					case 0x12: // MFLO
					case 0x28: // MFSA
						clear(rd);
						return;
					case 0x2a: // SLT
					case 0x2b: // SLTU
	{
						u32 lhs_low = 0;
						u32 lhs_high = 0;
						u32 rhs_low = 0;
						u32 rhs_high = 0;
						if (known64(rs, &lhs_low, &lhs_high) && known64(rt, &rhs_low, &rhs_high))
						{
							const u64 lhs = (static_cast<u64>(lhs_high) << 32) | lhs_low;
							const u64 rhs = (static_cast<u64>(rhs_high) << 32) | rhs_low;
							const bool result = (op & 0x3f) == 0x2a ?
								(static_cast<s64>(lhs) < static_cast<s64>(rhs)) :
								(lhs < rhs);
							set_zero32(rd, result ? 1 : 0);
						}
						else
						{
							clear(rd);
						}
						return;
	}
					default:
						return;
	}
			case 0x01:
				if (rt == 0x10 || rt == 0x11 || rt == 0x12 || rt == 0x13)
					set64(31, pc + 8, 0);
				return;
			case 0x03: // JAL
				set64(31, pc + 8, 0);
				return;
			case 0x08: // ADDI
			case 0x09: // ADDIU
				immediate_sign32(rt, rs, static_cast<u32>(static_cast<s32>(IMM_S(op))),
					[](u32 lhs, u32 rhs) { return lhs + rhs; });
				return;
			case 0x18: // DADDI
			case 0x19: // DADDIU
			{
				const u32 imm_low = static_cast<u32>(static_cast<s32>(IMM_S(op)));
				const u32 imm_high = (IMM_S(op) < 0) ? 0xffffffffu : 0;
				if (set_add64(rt, rs, imm_low, imm_high))
					return;
				immediate_low(rt, rs, imm_low, [](u32 lhs, u32 rhs) { return lhs + rhs; });
				return;
			}
			case 0x0a: // SLTI
			case 0x0b: // SLTIU
			{
				u32 lhs_low = 0;
				u32 lhs_high = 0;
				if (known64(rs, &lhs_low, &lhs_high))
	{
					const u64 lhs = (static_cast<u64>(lhs_high) << 32) | lhs_low;
					const s32 imm = static_cast<s32>(IMM_S(op));
					const u64 imm_value = (static_cast<u64>((imm < 0) ? 0xffffffffu : 0) << 32) |
										  static_cast<u32>(imm);
					const bool result = (op >> 26) == 0x0a ?
						(static_cast<s64>(lhs) < static_cast<s64>(imm)) :
						(lhs < imm_value);
					set_zero32(rt, result ? 1 : 0);
	}
				else
	{
					clear(rt);
	}
				return;
			}
			case 0x0c: // ANDI
			{
				u32 src_value = 0;
				if (known(rs, &src_value))
					set_zero32(rt, src_value & IMM_U(op));
				else
					clear(rt);
				return;
			}
			case 0x0d: // ORI
			{
				u32 src_low = 0;
				u32 src_high = 0;
				if (known64(rs, &src_low, &src_high))
					set64(rt, src_low | IMM_U(op), src_high);
				else
					immediate_low(rt, rs, IMM_U(op), [](u32 lhs, u32 rhs) { return lhs | rhs; });
				return;
			}
			case 0x0e: // XORI
			{
				u32 src_low = 0;
				u32 src_high = 0;
				if (known64(rs, &src_low, &src_high))
					set64(rt, src_low ^ IMM_U(op), src_high);
				else
					immediate_low(rt, rs, IMM_U(op), [](u32 lhs, u32 rhs) { return lhs ^ rhs; });
				return;
			}
			case 0x0f: // LUI
				set_sign32(rt, op << 16);
				return;
			case 0x1a: // LDL
			case 0x1b: // LDR
			case 0x1e: // LQ
			case 0x20: // LB
			case 0x21: // LH
			case 0x22: // LWL
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
			case 0x26: // LWR
			case 0x27: // LWU
			case 0x37: // LD
				clear(rt);
				return;
			case 0x10: // COP0 MFC0 writes rt; other accepted forms write no GPR.
				if (((op >> 21) & 0x1f) == 0x00)
					clear(rt);
				return;
			case 0x11: // COP1 MFC1/CFC1 write rt.
				if (((op >> 21) & 0x1f) == 0x00 || ((op >> 21) & 0x1f) == 0x02)
					clear(rt);
				return;
			case 0x12: // COP2 QMFC2/CFC2 write rt.
				if (((op >> 21) & 0x1f) == 0x01 || ((op >> 21) & 0x1f) == 0x02)
					clear(rt);
				return;
			case 0x1c: // MMI writers target rd when they write a GPR.
				clear(rd);
				return;
			default:
				return;
		}
	}

	void BlockCompiler::UpdateCop1NormalizedStateAfterOpcode(u32 op)
	{
		const bool acc_was_normalized = m_cop1_acc_normalized;
		const bool fs_was_normalized = IsCop1FprNormalized(RD(op));
		const bool ft_was_normalized = IsCop1FprNormalized(RT(op));
		const auto clear_fpr = [this](unsigned fpr) {
			if (fpr < 32)
				m_cop1_fpr_normalized[fpr] = false;
		};
		const auto mark_fpr = [this](unsigned fpr) {
			if (fpr < 32)
				m_cop1_fpr_normalized[fpr] = true;
		};
		const auto set_fpr = [this](unsigned fpr, bool normalized) {
			if (fpr < 32)
				m_cop1_fpr_normalized[fpr] = normalized;
		};

		switch (op >> 26)
		{
			case 0x11: // COP1, owned by FPU.cpp and x86/iFPU.cpp.
				switch ((op >> 21) & 0x1f)
	{
					case 0x00: // MFC1
					case 0x02: // CFC1
					case 0x06: // CTC1
					case 0x08: // BC1*
						return;
					case 0x04: // MTC1 writes a raw word into an FPR.
						clear_fpr(RD(op));
						return;
					case 0x10: // COP1_S
						switch (op & 0x3f)
						{
							case 0x00: // ADD_S
							case 0x01: // SUB_S
							case 0x02: // MUL_S
							case 0x03: // DIV_S
							case 0x04: // SQRT_S
							case 0x16: // RSQRT_S
								mark_fpr(SA(op));
								return;
							case 0x18: // ADDA_S
							case 0x19: // SUBA_S
							case 0x1a: // MULA_S
								m_cop1_acc_normalized = true;
								return;
							case 0x1e: // MADDA_S
							case 0x1f: // MSUBA_S
								m_cop1_acc_normalized = acc_was_normalized;
								return;
							case 0x1c: // MADD_S
							case 0x1d: // MSUB_S
								mark_fpr(SA(op));
								return;
							case 0x05: // ABS_S
							case 0x06: // MOV_S
							case 0x07: // NEG_S
								set_fpr(SA(op), fs_was_normalized);
								return;
							case 0x28: // MAX_S
							case 0x29: // MIN_S
								set_fpr(SA(op), fs_was_normalized && ft_was_normalized);
								return;
							case 0x24: // CVT_W
								clear_fpr(SA(op));
								return;
							case 0x30: // C_F
							case 0x32: // C_EQ
							case 0x34: // C_LT
							case 0x36: // C_LE
								return;
							default:
								ClearCop1NormalizedState();
								return;
						}
					case 0x14: // COP1_W
						if ((op & 0x3f) == 0x20) // CVT_S
						{
							mark_fpr(SA(op));
							return;
						}
						ClearCop1NormalizedState();
						return;
					default:
						ClearCop1NormalizedState();
						return;
	}
			case 0x31: // LWC1 writes a raw memory word into an FPR.
				clear_fpr(RT(op));
				return;
			default:
				return;
		}
	}

	void BlockCompiler::StageGprPinsForBlock(u32 start_pc, u32 instruction_count, bool allow_r5,
		bool allow_r7, bool allow_r8,
		bool allow_r10, bool allow_r11, bool prefer_dirty_writes,
		bool preserve_self_link_state)
	{
		m_staged_pin_count = 0;

		u16 read_counts[32]{};
		u16 dword_read_counts[32]{};
		u16 write_counts[32]{};
		u16 dword_write_counts[32]{};
		u32 entry_defined = 1;
		u32 entry_live_in_reads = 0;
		u32 self_link_defined = 1;
		u32 self_link_live_in_reads = 0;
		bool scanning_entry_liveness = prefer_dirty_writes;
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 op = memRead32(start_pc + i * 4);
			if (scanning_entry_liveness)
			{
				scanning_entry_liveness =
					UpdateGprPinEntryLiveness(op, entry_defined, entry_live_in_reads);
			}

			GprPinOpInfo info;
			if (!ClassifyOpcodeForGprPinning(op, &info))
				return;

			for (unsigned read = 0; read < info.low_read_count; read++)
			{
				const unsigned guest_reg = info.low_reads[read];
				read_counts[guest_reg]++;
				if ((self_link_defined & (1u << guest_reg)) == 0)
					self_link_live_in_reads |= 1u << guest_reg;
			}
			for (unsigned read = 0; read < info.dword_read_count; read++)
			{
				const unsigned guest_reg = info.dword_reads[read];
				read_counts[guest_reg]++;
				dword_read_counts[guest_reg]++;
				if ((self_link_defined & (1u << guest_reg)) == 0)
					self_link_live_in_reads |= 1u << guest_reg;
			}

			if (prefer_dirty_writes)
			{
				DirtyGprPinOpInfo dirty_info;
				if (!ClassifyOpcodeForDirtyGprPins(op, &dirty_info))
					return;

				for (unsigned write = 0; write < dirty_info.write_count; write++)
				{
					const unsigned guest_reg = dirty_info.writes[write];
					write_counts[guest_reg]++;
					dword_write_counts[guest_reg]++;
					self_link_defined |= 1u << guest_reg;
				}
			}
		}
		const u32 dead_entry_values = entry_defined & ~entry_live_in_reads;

		u8 hosts[MAX_GPR_PINS];
		unsigned host_count = 0;
		hosts[host_count++] = HOST_GPR_PIN0;
		if (allow_r5)
			hosts[host_count++] = HOST_BRANCH_STATE;
		if (allow_r10)
			hosts[host_count++] = HOST_COP1_EXPONENT_MASK;
		if (allow_r11)
			hosts[host_count++] = HOST_VU0_BASE;
		if (allow_r7)
			hosts[host_count++] = HOST_VTLB_VMAP;
		if (allow_r8)
			hosts[host_count++] = HOST_VTLB_HOST_MEMORY_BASE;

		bool host_used[MAX_GPR_PINS]{};
		unsigned used_hosts = 0;
		const auto find_free_host_slot = [&](unsigned host_reg) {
			for (unsigned slot = 0; slot < host_count; slot++)
			{
				if (!host_used[slot] && hosts[slot] == host_reg)
					return slot;
			}
			return host_count;
		};
		const auto next_free_host_slot = [&]() {
			for (unsigned slot = 0; slot < host_count; slot++)
			{
				if (!host_used[slot])
					return slot;
			}
			return host_count;
		};
		const auto next_free_dual_host_slots = [&](unsigned* low_slot, unsigned* high_slot) {
			if (!low_slot || !high_slot)
				return false;

			for (unsigned slot = 0; slot < host_count; slot++)
			{
				if (host_used[slot])
					continue;

				const unsigned low_host = hosts[slot];
				const unsigned high_host = low_host + 1;
				if (!CanUseA32DualTransferPair(low_host, high_host))
					continue;

				const unsigned candidate_high_slot = find_free_host_slot(high_host);
				if (candidate_high_slot < host_count)
	{
					*low_slot = slot;
					*high_slot = candidate_high_slot;
					return true;
	}
			}

			*low_slot = next_free_host_slot();
			if (*low_slot >= host_count)
				return false;
			host_used[*low_slot] = true;
			*high_slot = next_free_host_slot();
			host_used[*low_slot] = false;
			return *high_slot < host_count;
		};

		// A read-before-write value carried by an exact native self-edge repays
		// its entry load across iterations. Rank both loop-carried writers and
		// read-only invariants ahead of block-local reuse without changing the
		// ordinary three-write profitability threshold.
		constexpr u16 SELF_LINK_RESIDENCY_PRIORITY = 0x4000u;
		while (used_hosts + 1 < host_count)
		{
			unsigned best_reg = 0;
			u16 best_count = 1; // one low64 read only trades the entry loads for moves
			for (unsigned reg = 1; reg < 32; reg++)
			{
				const bool self_carried_write = preserve_self_link_state &&
					dword_write_counts[reg] != 0 &&
					(self_link_live_in_reads & (1u << reg)) != 0;
				const bool self_carried_invariant = preserve_self_link_state &&
					dword_read_counts[reg] != 0 && write_counts[reg] == 0 &&
					(self_link_live_in_reads & (1u << reg)) != 0;
				const u16 write_score = (self_carried_write || self_carried_invariant) ?
					static_cast<u16>(SELF_LINK_RESIDENCY_PRIORITY + dword_write_counts[reg]) :
					(dword_write_counts[reg] >= 3 ? dword_write_counts[reg] : 0);
				const u16 score = (dword_read_counts[reg] > write_score) ?
									  dword_read_counts[reg] :
									  write_score;
				if (score > best_count)
	{
					best_count = score;
					best_reg = reg;
	}
			}

			if (best_reg != 0)
			{
				unsigned low_slot = host_count;
				unsigned high_slot = host_count;
				if (!next_free_dual_host_slots(&low_slot, &high_slot))
					break;

				host_used[low_slot] = true;
				m_staged_pin_guest[m_staged_pin_count] = static_cast<u8>(best_reg);
				m_staged_pin_host[m_staged_pin_count] = hosts[low_slot];
				m_staged_pin_high_host[m_staged_pin_count] = hosts[high_slot];
				m_staged_pin_needs_entry_load[m_staged_pin_count] =
					(dead_entry_values & (1u << best_reg)) == 0;
				m_staged_pin_count++;
				host_used[high_slot] = true;
				used_hosts += 2;
				read_counts[best_reg] = 0;
				dword_read_counts[best_reg] = 0;
				write_counts[best_reg] = 0;
				dword_write_counts[best_reg] = 0;
			}
			else
			{
				break;
			}
		}

		for (unsigned slot = 0; slot < host_count && used_hosts < host_count; slot++)
		{
			if (host_used[slot])
				continue;

			unsigned best_reg = 0;
			u16 best_count = 1; // a single read only trades the entry load for the read
			for (unsigned reg = 1; reg < 32; reg++)
			{
				const bool self_carried_write = preserve_self_link_state &&
					write_counts[reg] != 0 &&
					(self_link_live_in_reads & (1u << reg)) != 0;
				const bool self_carried_invariant = preserve_self_link_state &&
					read_counts[reg] != 0 && write_counts[reg] == 0 &&
					(self_link_live_in_reads & (1u << reg)) != 0;
				const u16 write_score = (self_carried_write || self_carried_invariant) ?
					static_cast<u16>(SELF_LINK_RESIDENCY_PRIORITY + write_counts[reg]) :
					(write_counts[reg] >= 3 ? write_counts[reg] : 0);
				const u16 score = (read_counts[reg] > write_score) ? read_counts[reg] : write_score;
				if (score > best_count)
	{
					best_count = score;
					best_reg = reg;
	}
			}

			if (best_reg == 0)
				break;

			read_counts[best_reg] = 0;
			write_counts[best_reg] = 0;
			m_staged_pin_guest[m_staged_pin_count] = static_cast<u8>(best_reg);
			m_staged_pin_host[m_staged_pin_count] = hosts[slot];
			m_staged_pin_high_host[m_staged_pin_count] = NO_GPR_PIN_HOST;
			m_staged_pin_needs_entry_load[m_staged_pin_count] =
				(dead_entry_values & (1u << best_reg)) == 0;
			m_staged_pin_count++;
			host_used[slot] = true;
			used_hosts++;
		}
	}

	void BlockCompiler::StageGprPinsForLinkSignature(u32 start_pc, u32 instruction_count,
		const GprLinkSignature& signature)
	{
		if (!signature.IsValid())
			return;

		// These hosts are part of the persistent chain ABI and are available only
		// because BuildGprLinkSignature() rejects COP1/COP2 users. Width, host,
		// dirty ownership, representation, and provenance are all part of the
		// signature checked by the direct linker.
		static_assert(GprLinkSignature::FIRST_HOST == HOST_VTLB_VMAP);
		static_assert(GprLinkSignature::DEFAULT_FIRST_HOST == HOST_GPR_PIN0);
		static_assert(GprLinkSignature::LAST_CALLEE_HOST == HOST_VU0_BASE);
		static_assert(GprLinkSignature::LINK_REGISTER_HOST == HOST_LR);
		u32 defined = 1;
		u32 live_in_reads = 0;
		bool scanning = true;
		for (u32 i = 0; i < instruction_count && scanning; i++)
		{
			const u32 op = memRead32(start_pc + i * sizeof(u32));
			switch (op >> 26)
			{
				case 0x20: // LB
				case 0x21: // LH
				case 0x23: // LW
				case 0x24: // LBU
				case 0x25: // LHU
				case 0x27: // LWU
				case 0x37: // LD
				{
					// Unlike dirty block-local pins, this link ABI keeps backing
					// authoritative before the fault-capable vTLB operation. A
					// helper can therefore observe the old RT in backing without
					// requiring its clean host copy to be initialized; a successful
					// load overwrites RT before any generated use.
					const u32 base_bit = 1u << RS(op);
					if ((defined & base_bit) == 0)
						live_in_reads |= base_bit;
					defined |= 1u << RT(op);
					break;
				}
				default:
					scanning = UpdateGprPinEntryLiveness(op, defined, live_in_reads);
					break;
			}
		}
		const u32 dead_entry_values = defined & ~live_in_reads;

		m_staged_pin_count = signature.count;
		for (u8 i = 0; i < signature.count; i++)
		{
			const GprLinkMapping& mapping = signature.mappings[i];
			m_staged_pin_guest[i] = mapping.guest;
			m_staged_pin_host[i] = mapping.low_host;
			m_staged_pin_high_host[i] = mapping.width == GprLinkWidth::Low64 ?
				mapping.high_host : NO_GPR_PIN_HOST;
			m_staged_pin_needs_entry_load[i] =
				(dead_entry_values & (1u << mapping.guest)) == 0;
		}
	}

	void BlockCompiler::StageGprQCacheForBlock(u32 start_pc, u32 instruction_count)
	{
		constexpr unsigned MAX_STAGED_GPR_QCACHE_ENTRY_LOADS = 4;

		m_staged_gpr_q_cache_count = 0;
		m_gpr_q_cache_next_use_distances.clear();
		if (!m_gpr_q_cache_enabled)
			return;

		// PCSX2 owner: x86/iR5900Analysis.cpp::recBackpropBSC(). Build the
		// qword subset of its backward GPR liveness once, so allocation can evict
		// the resident value whose next 128-bit read is farthest away without
		// repeatedly rescanning the remainder of the block.
		constexpr u16 NO_NEXT_QWORD_USE = UINT16_MAX;
		m_gpr_q_cache_next_use_distances.resize(
			static_cast<size_t>(instruction_count) * 32, NO_NEXT_QWORD_USE);
		u32 next_read_index[32];
		for (u32& index : next_read_index)
			index = UINT32_MAX;
		for (u32 i = instruction_count; i-- > 0;)
		{
			for (unsigned reg = 1; reg < 32; reg++)
			{
				if (next_read_index[reg] != UINT32_MAX)
				{
					m_gpr_q_cache_next_use_distances[static_cast<size_t>(i) * 32 + reg] =
						static_cast<u16>(std::min<u32>(next_read_index[reg] - i,
							NO_NEXT_QWORD_USE - 1));
				}
			}

			u16 reads[32]{};
			u32 writes = 0;
			CountGprQCacheEntryQwordUses(memRead32(start_pc + i * 4), reads, writes);
			for (unsigned reg = 1; reg < 32; reg++)
			{
				if ((writes & (1u << reg)) != 0)
					next_read_index[reg] = UINT32_MAX;
				if (reads[reg] != 0)
					next_read_index[reg] = i;
			}
		}

		u16 qword_read_counts[32]{};
		u16 variable_shift_rt_reads[32]{};
		u32 defined = 1;
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 op = memRead32(start_pc + i * 4);
			if ((op >> 26) == 0x1c)
			{
				// PCSX2 owner: x86/iMMI.cpp::recPSLLVW()/recPSRLVW()/recPSRAVW().
				// Their RT operand is the shifted qword; RS supplies shift counts.
				const unsigned funct = op & 0x3f;
				const unsigned sub = (op >> 6) & 0x1f;
				const bool is_variable_word_shift =
					(funct == 0x09 && (sub == 0x02 || sub == 0x03)) ||
					(funct == 0x29 && sub == 0x03);
				const unsigned rt = RT(op);
				if (is_variable_word_shift && rt != 0 &&
					(defined & (1u << rt)) == 0)
				{
					variable_shift_rt_reads[rt]++;
				}
			}
			CountGprQCacheEntryQwordUses(op, qword_read_counts, defined);
		}

		bool staged[32]{};
		for (;;)
		{
			unsigned best_reg = 0;
			const u16 minimum_profitable_count =
				m_staged_gpr_q_cache_count < 2 ? 2 : 3;
			u16 best_count = static_cast<u16>(minimum_profitable_count - 1);
			for (unsigned reg = 1; reg < 32; reg++)
			{
				if (staged[reg])
					continue;

				if (qword_read_counts[reg] > best_count ||
					(qword_read_counts[reg] == best_count &&
					 variable_shift_rt_reads[reg] >
						 variable_shift_rt_reads[best_reg]))
		{
					best_count = qword_read_counts[reg];
					best_reg = reg;
		}
			}

			if (best_reg == 0 ||
				m_staged_gpr_q_cache_count >= MAX_STAGED_GPR_QCACHE_ENTRY_LOADS)
			{
				break;
			}

			staged[best_reg] = true;
			m_staged_gpr_q_cache_guest[m_staged_gpr_q_cache_count++] =
				static_cast<u8>(best_reg);
		}
	}

	bool BlockCompiler::BlockWritesPinnedGpr(u32 start_pc, u32 instruction_count) const
	{
		for (u32 i = 0; i < instruction_count; i++)
		{
			DirtyGprPinOpInfo info;
			if (!ClassifyOpcodeForDirtyGprPins(memRead32(start_pc + i * 4), &info))
				return false;

			for (unsigned write = 0; write < info.write_count; write++)
			{
				if (FindGprPinIndex(info.writes[write]) >= 0)
					return true;
			}
		}

		return false;
	}

	void BlockCompiler::MarkGprPinsDirtyAtResidentSelfLinkEntry(u32 start_pc,
		u32 instruction_count)
	{
		// PCSX2 iCore MODE_WRITE mappings remain authoritative in their host
		// registers until an explicit writeback seam. Compilation begins at the
		// canonical clean entry, but an exact resident self-edge reaches the same
		// body with the previous iteration's deferred values still in its pins.
		// Mark every pinned block writer dirty from that join so a memory-handler
		// cold edge before the first current-iteration write synchronizes the
		// inherited value.
		for (u32 i = 0; i < instruction_count; i++)
		{
			DirtyGprPinOpInfo info;
			if (!ClassifyOpcodeForDirtyGprPins(memRead32(start_pc + i * 4), &info))
				return;

			for (unsigned write = 0; write < info.write_count; write++)
			{
				const int pin_index = FindGprPinIndex(info.writes[write]);
				if (pin_index < 0)
					continue;

				m_pin_dirty_low[pin_index] = true;
				if (m_pin_high_host[pin_index] != NO_GPR_PIN_HOST)
					m_pin_dirty_high[pin_index] = true;
			}
		}
	}

	int BlockCompiler::FindGprPinIndex(unsigned guest_reg) const
	{
		for (unsigned i = 0; i < m_pin_count; i++)
		{
			if (m_pin_guest[i] == guest_reg)
				return static_cast<int>(i);
		}

		return -1;
	}

	int BlockCompiler::FindGprPinHost(unsigned guest_reg) const
	{
		for (unsigned i = 0; i < m_pin_count; i++)
		{
			if (m_pin_guest[i] == guest_reg)
				return static_cast<int>(m_pin_host[i]);
		}

		return -1;
	}

	int BlockCompiler::FindGprPinHighHost(unsigned guest_reg) const
	{
		for (unsigned i = 0; i < m_pin_count; i++)
		{
			if (m_pin_guest[i] == guest_reg && m_pin_high_host[i] != NO_GPR_PIN_HOST)
				return static_cast<int>(m_pin_high_host[i]);
		}

		return -1;
	}

	u8 BlockCompiler::GprPinEntryLoadInstructionCount() const
	{
		u8 count = 0;
		for (unsigned i = 0; i < m_pin_count; i++)
		{
			if (!m_pin_needs_entry_load[i])
				continue;

			const size_t offset = GprOffset(m_pin_guest[i]);
			count += (m_pin_high_host[i] != NO_GPR_PIN_HOST &&
				(offset > 0xff || !CanUseA32DualTransferPair(m_pin_host[i], m_pin_high_host[i]))) ?
				2 : 1;
		}
		return count;
	}

	bool BlockCompiler::EmitGprPinLoads()
	{
		for (unsigned i = 0; i < m_pin_count; i++)
			{
				const size_t offset = GprOffset(m_pin_guest[i]);
				if (!m_pin_needs_entry_load[i])
	{
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuGprPinEntryLoadInstructionsElided +=
						(m_pin_high_host[i] != NO_GPR_PIN_HOST &&
							(offset > 0xff || !CanUseA32DualTransferPair(m_pin_host[i], m_pin_high_host[i]))) ?
						2 : 1;
#endif
					continue;
	}

				if (m_pin_high_host[i] != NO_GPR_PIN_HOST &&
					offset <= 0xff &&
					CanUseA32DualTransferPair(m_pin_host[i], m_pin_high_host[i]))
	{
					if (!m_code.EmitLdrdImm8(m_pin_host[i], m_pin_high_host[i], HOST_CPU_REGS,
						static_cast<u8>(offset)))
	{
						return false;
	}
					continue;
	}

				if (!m_code.EmitLdrImm12(m_pin_host[i], HOST_CPU_REGS, static_cast<u16>(offset)))
					return false;

				if (m_pin_high_host[i] != NO_GPR_PIN_HOST &&
					!m_code.EmitLdrImm12(m_pin_high_host[i], HOST_CPU_REGS,
						static_cast<u16>(offset + sizeof(u32))))
	{
					return false;
	}
			}

		return true;
	}

	bool BlockCompiler::EmitGprQCacheEntryLoads()
	{
		for (unsigned i = 0; i < m_staged_gpr_q_cache_count; i++)
		{
			const unsigned guest_reg = m_staged_gpr_q_cache_guest[i];
			const unsigned qreg = i;
			if (!EmitLoadCpuRegsQ128(GprOffset(guest_reg), qreg, HOST_TMP0))
				return false;

			MarkGprQCache(guest_reg, qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheEntryLoads++;
#endif
		}

		return true;
	}

	bool BlockCompiler::EmitFlushDirtyGprPins()
	{
		if (!m_dirty_pins_enabled)
			return true;

			for (unsigned i = 0; i < m_pin_count; i++)
			{
				const size_t offset = GprOffset(m_pin_guest[i]);
				if (m_pin_dirty_low[i] && m_pin_dirty_high[i] &&
					m_pin_high_host[i] != NO_GPR_PIN_HOST &&
					offset <= 0xff &&
					CanUseA32DualTransferPair(m_pin_host[i], m_pin_high_host[i]))
	{
					if (!m_code.EmitStrdImm8(m_pin_host[i], m_pin_high_host[i], HOST_CPU_REGS,
						static_cast<u8>(offset)))
	{
						return false;
	}
					m_pin_dirty_low[i] = false;
					m_pin_dirty_high[i] = false;
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuGprDirtyPinFlushStores += 2;
#endif
					continue;
	}

				if (m_pin_dirty_low[i])
	{
					if (!m_code.EmitStrImm12(m_pin_host[i], HOST_CPU_REGS, static_cast<u16>(offset)))
	{
						return false;
	}
					m_pin_dirty_low[i] = false;
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuGprDirtyPinFlushStores++;
#endif
	}

				if (m_pin_dirty_high[i])
	{
					if (m_pin_high_host[i] == NO_GPR_PIN_HOST ||
						!m_code.EmitStrImm12(m_pin_high_host[i], HOST_CPU_REGS,
							static_cast<u16>(offset + sizeof(u32))))
	{
						return false;
	}
					m_pin_dirty_high[i] = false;
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuGprDirtyPinFlushStores++;
#endif
	}
			}

		return true;
	}

	BlockCompiler::GprPinDirtyMasks BlockCompiler::CurrentGprPinDirtyMasks() const
	{
		GprPinDirtyMasks masks;
		for (unsigned i = 0; i < m_pin_count; i++)
		{
			masks.low |= static_cast<u8>(m_pin_dirty_low[i] ? (1u << i) : 0);
			masks.high |= static_cast<u8>(m_pin_dirty_high[i] ? (1u << i) : 0);
		}
		return masks;
	}

	bool BlockCompiler::IsForwardedBooleanBranchResult(unsigned guest_reg) const
	{
		return m_forwarded_boolean_branch && guest_reg == m_forwarded_boolean_guest &&
			m_current_instruction_index == m_forwarded_boolean_producer_index;
	}

	bool BlockCompiler::EmitStageResidentRawGpr0Qword()
	{
		if (!m_resident_raw_gpr0_qword)
			return true;

		// PCSX2 owner: iR5900LoadStore.cpp::recStore(128) obtains the raw SQ
		// source through iCore's persistent MODE_READ XMM mapping. Establish the
		// same raw GPR0 value once at canonical entry, including the LD-$zero
		// compatibility case, then let the exact resident self-edge retain q0.
		constexpr unsigned NEON_VALUE = 0;
		const size_t prelude_start = m_code.Size();
		if (!EmitLoadRawGpr0KnownZeroFlag(HOST_TMP1) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1,
				VitaA32::ShiftType::LSL, 0, true))
		{
			return false;
		}

		const size_t raw_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (raw_fallback == static_cast<size_t>(-1) ||
			!m_code.EmitVeorQ(NEON_VALUE, NEON_VALUE, NEON_VALUE))
		{
			return false;
		}

		const size_t prelude_done = m_code.EmitBranchPlaceholder();
		if (prelude_done == static_cast<size_t>(-1))
			return false;

		const size_t raw_fallback_target = m_code.Size();
		const size_t hot_instructions =
			(raw_fallback_target - prelude_start) / sizeof(u32);
		if (hot_instructions > UINT8_MAX ||
			!m_code.PatchBranch(raw_fallback, raw_fallback_target,
				VitaA32::Condition::EQ) ||
			!EmitLoadCpuRegsQ128(GprOffset(0), NEON_VALUE, HOST_TMP1) ||
			!m_code.PatchBranch(prelude_done, m_code.Size()))
		{
			return false;
		}

		m_resident_raw_gpr0_entry_instructions = static_cast<u8>(hot_instructions);
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuResidentRawGpr0QwordBlocks++;
#endif
		return true;
	}

	bool BlockCompiler::EmitStageResidentVtlbQwordPointer()
	{
		if (!m_resident_vtlb_qword_pointer)
			return true;

		// PCSX2 owner: recVTLB.cpp::vtlb_DynGenWrite() reduces a fastmem SQ to
		// one indexed host store and recovers faults out of line. Vita cannot
		// reserve a 4 GiB fastmem window, so retain the translated qword pointer
		// in caller-saved r3 across this exact self-edge and re-run vTLB only when
		// its post-store page offset is zero. Direct VTLB mappings preserve the
		// guest page offset in their page-aligned host pointer. Handler tails poison
		// r3 to zero, so the same offset guard forces retranslation.
		const size_t canonical_retranslate = m_code.EmitBranchPlaceholder();
		if (canonical_retranslate == static_cast<size_t>(-1))
			return false;

		m_resident_vtlb_qword_guard_offset = m_code.Size();
		const size_t guard_start = m_code.Size();
		if (!m_code.EmitTstImm32(HOST_TMP3, vtlb_private::VTLB_PAGE_MASK & ~0x0fu))
		{
			return false;
		}
		const size_t same_page = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (same_page == static_cast<size_t>(-1))
			return false;
		const size_t guard_end = m_code.Size();
		if (!EmitSaveResidentSchedulerCountdown())
			return false;
		if (m_resident_forwarded_boolean_mask)
		{
			// q1 lane 1 distinguishes a resident page/handler retranslation from
			// canonical entry. r4 is the persistent non-null cpuRegs base; the
			// canonical scheduler prelude cleared the lane to zero.
			const size_t marker_start = m_code.Size();
			if (!EmitMoveCoreToQWordLane(1, 1, HOST_CPU_REGS))
				return false;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuResidentForwardedBooleanMaskPageInstructions += static_cast<u32>(
				(m_code.Size() - marker_start) / sizeof(u32));
#endif
		}

		const size_t translation_start = m_code.Size();
		if (!m_code.PatchBranch(canonical_retranslate, translation_start) ||
			!EmitEffectiveAddress(m_resident_vtlb_qword_store_op, HOST_TMP3) ||
			!EmitAlignQwordAddress(HOST_TMP3, HOST_TMP1) ||
			!EmitVtlbNonHandlerHostAddress128(HOST_TMP3, HOST_TMP1, HOST_TMP2,
				&m_resident_vtlb_qword_handler_fallback,
				&m_resident_vtlb_qword_dirty_pins))
		{
			return false;
		}

		const size_t translation_end = m_code.Size();
		if (!EmitRestoreResidentSchedulerCountdown())
			return false;
		// PCSX2 owner: iCore.cpp keeps the zero-extended high half of the
		// recSLTU() MODE_WRITE mapping resident. Canonical and page-translation
		// entries must establish that invariant only after a direct mapping has
		// been selected: a handler edge still needs the incoming architectural
		// high word when it synchronizes cpuRegs before calling PCSX2.
		const size_t high_zero_start = m_code.Size();
		if (m_forwarded_boolean_branch && !m_code.EmitMovImm8(HOST_TMP5, 0))
			return false;
		const size_t body_start = m_code.Size();
		const size_t guard_instructions =
			(guard_end - guard_start) / sizeof(u32);
		const size_t translation_instructions =
			(translation_end - translation_start) / sizeof(u32);
		if (guard_instructions > UINT8_MAX || translation_instructions > UINT8_MAX ||
			!m_code.PatchBranch(same_page, body_start, VitaA32::Condition::NE))
		{
			return false;
		}

		m_resident_vtlb_qword_guard_instructions = static_cast<u8>(guard_instructions);
		m_resident_vtlb_qword_translation_instructions =
			static_cast<u8>(translation_instructions);
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuResidentVtlbQwordPointerBlocks++;
		g_qemuResidentVtlbQwordPointerGuardInstructions +=
			static_cast<u32>(guard_instructions);
		g_qemuResidentVtlbQwordPointerTranslationInstructions +=
			static_cast<u32>(translation_instructions);
		g_qemuResidentSchedulerCountdownPageCarryInstructions += 2;
		g_qemuResidentForwardedBooleanHighZeroTranslationInstructions +=
			static_cast<u32>((m_code.Size() - high_zero_start) / sizeof(u32));
#endif
		return true;
	}

	bool BlockCompiler::EmitSyncForwardedBooleanBranchToBacking(bool value_is_architectural)
	{
		if (!m_forwarded_boolean_branch)
			return true;

		const size_t offset = GprOffset(m_forwarded_boolean_guest);
		if (m_resident_forwarded_boolean_mask && !value_is_architectural)
		{
			const size_t normalize_start = m_code.Size();
			if (!m_code.EmitAndImm8(m_branch_flag_host, m_branch_flag_host, 1))
				return false;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuResidentForwardedBooleanMaskPublicationInstructions += static_cast<u32>(
				(m_code.Size() - normalize_start) / sizeof(u32));
#endif
		}
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuForwardedBooleanBranchSyncWords += 2;
#endif
		return m_code.EmitStrImm12(m_branch_flag_host, HOST_CPU_REGS, static_cast<u16>(offset)) &&
			m_code.EmitStrImm12(HOST_TMP5, HOST_CPU_REGS,
				static_cast<u16>(offset + sizeof(u32)));
	}

	bool BlockCompiler::EmitPrepareResidentForwardedBooleanForPreProducerSync()
	{
		if (!m_resident_forwarded_boolean_mask)
			return true;

		// Canonical entry may still carry an arbitrary architectural $at because
		// SQ precedes its SLTU producer. q1 lane 1 is zero on that path and nonzero
		// on every resident retranslation. Normalize the resident 0/-1 mask only;
		// leave the canonical value untouched for the helper snapshot.
		const size_t prepare_start = m_code.Size();
		if (!EmitMoveQWordLaneToCore(HOST_TMP1, 1, 1) ||
			!m_code.EmitCmpImm32(HOST_TMP1, 0))
		{
			return false;
		}
		const size_t canonical_value =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (canonical_value == static_cast<size_t>(-1) ||
			!m_code.EmitAndImm8(m_branch_flag_host, m_branch_flag_host, 1) ||
			!m_code.PatchBranch(canonical_value, m_code.Size(), VitaA32::Condition::EQ))
		{
			return false;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuResidentForwardedBooleanMaskHandlerInstructions += static_cast<u32>(
			(m_code.Size() - prepare_start) / sizeof(u32));
#endif
		return true;
	}

	bool BlockCompiler::EmitPoisonCompatibleVtlbPointer()
	{
		if (!m_compatible_vtlb_pointer)
			return true;

		// Canonical/dispatcher entries cannot inherit the private r12 mapping.
		// Compatible links jump past this poison after signature equality proves
		// the pointer's guest register, width, direction, and vTLB provenance.
		const size_t poison_start = m_code.Size();
		if (!m_code.EmitMovImm8(GprLinkSignature::VTLB_POINTER_HOST, 0))
			return false;
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuCompatibleVtlbPointerCanonicalPoisonInstructions += static_cast<u32>(
			(m_code.Size() - poison_start) / sizeof(u32));
#endif
		return true;
	}

	bool BlockCompiler::EmitStageCompatibleVtlbPointer()
	{
		if (!m_compatible_vtlb_pointer_access)
			return true;

		// PCSX2 owner: recVTLB.cpp::DynGen_PrepRegs()/DynGen_DirectRead(). r12
		// carries the direct host address across the reciprocal links. A direct
		// word load advances it by four; the guest ADDIU in the partner block then
		// catches the architectural address up. Direct VTLB mappings preserve the
		// guest page offset, so low 12 bits zero means canonical entry, a poisoned
		// helper result, or a page transition and forces exact retranslation.
		const size_t guard_start = m_code.Size();
		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, GprLinkSignature::VTLB_POINTER_HOST,
				VitaA32::ShiftType::LSL, 20, true))
		{
			return false;
		}
		const size_t same_page = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (same_page == static_cast<size_t>(-1))
			return false;
		const size_t guard_end = m_code.Size();

		const u32 op = memRead32(m_gpr_link_signature.vtlb_pointer.access_pc);
		const size_t translation_start = m_code.Size();
		if (!EmitEffectiveAddress(op, GprLinkSignature::VTLB_POINTER_HOST) ||
			!m_code.EmitAndImm8(HOST_TMP1, GprLinkSignature::VTLB_POINTER_HOST, 3, true))
		{
			return false;
		}
		m_compatible_vtlb_pointer_unaligned_fallback =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (m_compatible_vtlb_pointer_unaligned_fallback == static_cast<size_t>(-1) ||
			!EmitVtlbNonHandlerHostAddress(GprLinkSignature::VTLB_POINTER_HOST,
				HOST_TMP1, HOST_TMP2, &m_compatible_vtlb_pointer_handler_fallback,
				&m_compatible_vtlb_pointer_dirty_pins))
		{
			return false;
		}
		if (!EmitReloadGprPinsAfterClobber(static_cast<u16>(1u << HOST_TMP1)))
			return false;
		const size_t translation_end = m_code.Size();
		const size_t body_start = m_code.Size();
		if (!m_code.PatchBranch(same_page, body_start, VitaA32::Condition::NE))
			return false;

		const size_t guard_instructions = (guard_end - guard_start) / sizeof(u32);
		const size_t translation_instructions =
			(translation_end - translation_start) / sizeof(u32);
		if (guard_instructions > UINT8_MAX || translation_instructions > UINT8_MAX)
			return false;
		m_compatible_vtlb_pointer_guard_instructions =
			static_cast<u8>(guard_instructions);
		m_compatible_vtlb_pointer_translation_instructions =
			static_cast<u8>(translation_instructions);
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuCompatibleVtlbPointerBlocks++;
		g_qemuCompatibleVtlbPointerGuardInstructions +=
			static_cast<u32>(guard_instructions);
		g_qemuCompatibleVtlbPointerTranslationInstructions +=
			static_cast<u32>(translation_instructions);
		if (translation_instructions >= guard_instructions)
		{
			g_qemuCompatibleVtlbPointerHotInstructionsElided += static_cast<u32>(
				translation_instructions - guard_instructions);
		}
#endif
		return true;
	}

	bool BlockCompiler::EmitStageResidentSchedulerCountdown(bool preserve_for_translation)
	{
		if (!m_resident_scheduler_countdown)
			return true;

		// PCSX2 owner: x86/ix86-32/iR5900.cpp::iBranchTest() maintains private
		// scheduler state across recompiled dispatch. r0 retains nextEventCycle.low
		// while r2 retains the signed cycle.low-nextEventCycle.low countdown. Each
		// resident edge can then advance time and set the event predicate with one
		// flag-setting ADD. Canonical entry also saves r2 in q1 because the first
		// vTLB translation uses r2 as scratch before native code begins.
		const size_t stage_start = m_code.Size();
		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS,
				static_cast<u16>(NEXT_EVENT_OFFSET)) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS,
				static_cast<u16>(CYCLE_OFFSET)) ||
			!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP0) ||
			(preserve_for_translation && m_resident_forwarded_boolean_mask &&
			 !m_code.EmitVeorQ(1, 1, 1)) ||
			(preserve_for_translation && !EmitSaveResidentSchedulerCountdown()))
		{
			return false;
		}

#if defined(VITASX2_QEMU_VALIDATION)
		if (preserve_for_translation)
		{
			g_qemuResidentCycleLowBlocks++;
			g_qemuResidentNextEventLowBlocks++;
			g_qemuResidentSchedulerCountdownBlocks++;
			g_qemuResidentSchedulerCountdownCanonicalInstructions += static_cast<u32>(
				(m_code.Size() - stage_start) / sizeof(u32));
			if (m_resident_forwarded_boolean_mask)
				g_qemuResidentForwardedBooleanMaskCanonicalInstructions++;
		}
#endif
		return true;
	}

	bool BlockCompiler::EmitStageCompatibleSchedulerCountdown(unsigned scratch_host,
		bool canonical_entry)
	{
		if (!m_compatible_scheduler_countdown)
			return true;
		if (scratch_host == GprLinkSignature::SCHEDULER_HOST)
			return false;

		// Adapt PCSX2 x86/ix86-32/iR5900.cpp::iBranchTest() to the tighter A32
		// register file. r6 carries cycle.low-nextEventCycle.low across compatible
		// direct links; canonical entry reconstructs it from authoritative state.
		const size_t stage_start = m_code.Size();
		if (!m_code.EmitLdrImm12(GprLinkSignature::SCHEDULER_HOST, HOST_CPU_REGS,
				static_cast<u16>(CYCLE_OFFSET)) ||
			!m_code.EmitLdrImm12(scratch_host, HOST_CPU_REGS,
				static_cast<u16>(NEXT_EVENT_OFFSET)) ||
			!m_code.EmitSubReg(GprLinkSignature::SCHEDULER_HOST,
				GprLinkSignature::SCHEDULER_HOST, scratch_host))
		{
			return false;
		}

#if defined(VITASX2_QEMU_VALIDATION)
		const u32 instructions = static_cast<u32>(
			(m_code.Size() - stage_start) / sizeof(u32));
		if (canonical_entry)
		{
			g_qemuResidentCycleLowBlocks++;
			g_qemuResidentNextEventLowBlocks++;
			g_qemuResidentSchedulerCountdownBlocks++;
			g_qemuResidentSchedulerCountdownCanonicalInstructions += instructions;
		}
		else
		{
			g_qemuResidentCycleLowColdReloadInstructions++;
			g_qemuResidentNextEventLowColdReloadInstructions++;
			g_qemuResidentSchedulerCountdownHandlerInstructions += instructions;
		}
#endif
		return true;
	}

	bool BlockCompiler::EmitSyncCompatibleSchedulerCountdownToBacking()
	{
		if (!m_compatible_scheduler_countdown)
			return true;

		// Helpers and the dispatcher observe full cpuRegs.cycle. Reconstruct the
		// low word from the still-authoritative deadline, then repair the one
		// possible wrap since the last publication. An event must occur within the
		// scheduler's signed-32-bit window, so a compatible chain cannot cross two
		// low-word wraps without first reaching this seam.
		if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS,
				static_cast<u16>(NEXT_EVENT_OFFSET)) ||
			!m_code.EmitAddReg(HOST_TMP2, HOST_TMP1,
				GprLinkSignature::SCHEDULER_HOST) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS,
				static_cast<u16>(CYCLE_OFFSET)) ||
			!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP1))
		{
			return false;
		}

		const size_t no_wrap = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
		constexpr u16 cycle_high_offset = static_cast<u16>(CYCLE_OFFSET + sizeof(u32));
		if (no_wrap == static_cast<size_t>(-1) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, cycle_high_offset) ||
			!m_code.EmitAddImm8(HOST_TMP1, HOST_TMP1, 1) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, cycle_high_offset) ||
			!m_code.PatchBranch(no_wrap, m_code.Size(), VitaA32::Condition::CS) ||
			!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS,
				static_cast<u16>(CYCLE_OFFSET)))
		{
			return false;
		}

#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuResidentCycleLowSyncInstructions += 6;
		g_qemuResidentCycleLowWrapFixupInstructions += 3;
#endif
		return true;
	}

	bool BlockCompiler::EmitSyncResidentCycleLowToBacking()
	{
		if (m_compatible_scheduler_countdown)
			return EmitSyncCompatibleSchedulerCountdownToBacking();
		if (!m_resident_cycle_low)
			return true;
		if (m_resident_scheduler_countdown &&
			!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
		{
			return false;
		}

		// The scheduler maintains nextEventCycle within a signed-32-bit delta, so
		// at most one low-word wrap can occur before this event/helper/unlink seam.
		// Compare against the last published low word, repair the high word only on
		// wrap, then make the resident low word authoritative for external code.
		if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)) ||
			!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1))
		{
			return false;
		}

		const size_t no_wrap = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
		constexpr u16 cycle_high_offset = static_cast<u16>(CYCLE_OFFSET + sizeof(u32));
		if (no_wrap == static_cast<size_t>(-1) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, cycle_high_offset) ||
			!m_code.EmitAddImm8(HOST_TMP1, HOST_TMP1, 1) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, cycle_high_offset) ||
			!m_code.PatchBranch(no_wrap, m_code.Size(), VitaA32::Condition::CS) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)))
		{
			return false;
		}

#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuResidentCycleLowSyncInstructions +=
			m_resident_scheduler_countdown ? 5 : 4;
		g_qemuResidentCycleLowWrapFixupInstructions += 3;
#endif
		return true;
	}

	bool BlockCompiler::EmitSaveResidentSchedulerCountdown()
	{
		if (!m_resident_scheduler_countdown)
			return true;
		constexpr unsigned NEON_COUNTDOWN = 1;
		return EmitMoveCoreToQWordLane(NEON_COUNTDOWN, 0, HOST_TMP2);
	}

	bool BlockCompiler::EmitRestoreResidentSchedulerCountdown()
	{
		if (!m_resident_scheduler_countdown)
			return true;
		constexpr unsigned NEON_COUNTDOWN = 1;
		return EmitMoveQWordLaneToCore(HOST_TMP2, NEON_COUNTDOWN, 0);
	}

	bool BlockCompiler::EmitDeferredResidentUnsignedBranchSuffix(bool event_path)
	{
		if (!m_deferred_resident_unsigned_branch_suffix)
			return true;

		// PCSX2 owners: recSLTU() leaves its MODE_WRITE result available to
		// recBNE(), while recompileNextInstruction() emits the ADDIU delay slot
		// before iBranchTest(). The resident scheduler test is private host work,
		// so it may run first. Emit the guest suffix afterward: CMP/CMP/SBC leaves
		// unsigned-less in C, and ADDIU's ADD/ASR do not set flags. The self-link
		// can therefore consume CC directly without re-comparing the 0/-1 mask.
		const u32 previous_index = m_current_instruction_index;
		const bool previous_event_suffix = m_emitting_deferred_resident_event_suffix;
		m_emitting_deferred_resident_event_suffix = event_path;
		m_current_instruction_index = m_forwarded_boolean_producer_index;
		const bool compare_ok = EmitSLTU(m_deferred_resident_unsigned_compare_op);
		m_current_instruction_index = m_current_block_instruction_count - 1;
		const bool delay_ok = compare_ok && EmitADDIU(m_deferred_resident_delay_op);
		m_current_instruction_index = previous_index;
		m_emitting_deferred_resident_event_suffix = previous_event_suffix;
		return delay_ok;
	}

	bool BlockCompiler::EmitSyncGprPinsToBacking(const GprPinDirtyMasks* dirty_pins,
		bool forwarded_value_is_architectural)
	{
		if (!m_dirty_pins_enabled && !m_forwarded_boolean_branch &&
			!m_resident_cycle_low && !m_compatible_scheduler_countdown)
			return true;
		const GprPinDirtyMasks masks = m_dirty_pins_enabled ?
			(dirty_pins ? *dirty_pins : CurrentGprPinDirtyMasks()) : GprPinDirtyMasks{};

		// PCSX2 owners: x86/iCore.cpp::_deleteGPRtoX86reg() and
		// _deleteGPRtoXMMreg() write back only MODE_WRITE mappings. This sync is
		// emitted inside conditionally executed seams (helper cold tails,
		// exception tails in likely delay slots), so it must not
		// clear the compile-time dirty flags: the fall-through path still
		// needs the block-exit flush to store the deferred words. Deferred cold
		// tails carry the dirty snapshot from their branch point because they are
		// emitted after the hot block's compile-time flags have been cleared.
		for (unsigned i = 0; i < m_pin_count; i++)
		{
			const bool dirty_low = (masks.low & (1u << i)) != 0;
			const bool dirty_high = (masks.high & (1u << i)) != 0;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprPinColdSyncWordsElided += !dirty_low;
			g_qemuGprPinColdSyncWordsStored += dirty_low;
			if (m_pin_high_host[i] != NO_GPR_PIN_HOST)
			{
				g_qemuGprPinColdSyncWordsElided += !dirty_high;
				g_qemuGprPinColdSyncWordsStored += dirty_high;
			}
#endif
			if (!dirty_low && !dirty_high)
				continue;

			const size_t offset = GprOffset(m_pin_guest[i]);
			if (dirty_low && dirty_high &&
				m_pin_high_host[i] != NO_GPR_PIN_HOST &&
				offset <= 0xff &&
				CanUseA32DualTransferPair(m_pin_host[i], m_pin_high_host[i]))
			{
				if (!m_code.EmitStrdImm8(m_pin_host[i], m_pin_high_host[i], HOST_CPU_REGS,
					static_cast<u8>(offset)))
	{
					return false;
	}
				continue;
			}

			if (dirty_low &&
				!m_code.EmitStrImm12(m_pin_host[i], HOST_CPU_REGS, static_cast<u16>(offset)))
			{
				return false;
			}

			if (dirty_high &&
				(m_pin_high_host[i] == NO_GPR_PIN_HOST ||
				 !m_code.EmitStrImm12(m_pin_high_host[i], HOST_CPU_REGS,
					 static_cast<u16>(offset + sizeof(u32)))))
			{
				return false;
			}
		}

		return EmitSyncForwardedBooleanBranchToBacking(forwarded_value_is_architectural) &&
			EmitSyncResidentCycleLowToBacking();
	}

	bool BlockCompiler::EmitFlushDirtyGprPinsForGuest(unsigned guest_reg)
	{
		if (!m_dirty_pins_enabled)
			return true;

		const int pin_index = FindGprPinIndex(guest_reg);
		if (pin_index < 0)
			return true;

		const unsigned i = static_cast<unsigned>(pin_index);
		const size_t offset = GprOffset(guest_reg);
		if (m_pin_dirty_low[i] && m_pin_dirty_high[i] &&
			m_pin_high_host[i] != NO_GPR_PIN_HOST &&
			offset <= 0xff &&
			CanUseA32DualTransferPair(m_pin_host[i], m_pin_high_host[i]))
		{
			if (!m_code.EmitStrdImm8(m_pin_host[i], m_pin_high_host[i], HOST_CPU_REGS,
				static_cast<u8>(offset)))
			{
				return false;
			}
			m_pin_dirty_low[i] = false;
			m_pin_dirty_high[i] = false;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprDirtyPinFlushStores += 2;
#endif
			return true;
		}

		if (m_pin_dirty_low[i])
		{
			if (!m_code.EmitStrImm12(m_pin_host[i], HOST_CPU_REGS, static_cast<u16>(offset)))
				return false;
			m_pin_dirty_low[i] = false;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprDirtyPinFlushStores++;
#endif
		}

		if (m_pin_dirty_high[i])
		{
			if (m_pin_high_host[i] == NO_GPR_PIN_HOST ||
				!m_code.EmitStrImm12(m_pin_high_host[i], HOST_CPU_REGS,
					static_cast<u16>(offset + sizeof(u32))))
			{
				return false;
			}
			m_pin_dirty_high[i] = false;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprDirtyPinFlushStores++;
#endif
		}

		return true;
	}

	bool BlockCompiler::TryDeferGprPinLowStore(unsigned guest_reg)
	{
		if (!m_dirty_pins_enabled)
			return false;

		const int pin_index = FindGprPinIndex(guest_reg);
		if (pin_index < 0)
			return false;

		m_pin_dirty_low[pin_index] = true;
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprDirtyPinLowStoresElided++;
#endif
		return true;
	}

	bool BlockCompiler::TryDeferGprPinHighStore(unsigned guest_reg)
	{
		if (!m_dirty_pins_enabled)
			return false;

		const int pin_index = FindGprPinIndex(guest_reg);
		if (pin_index < 0 || m_pin_high_host[pin_index] == NO_GPR_PIN_HOST)
			return false;

		m_pin_dirty_high[pin_index] = true;
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprDirtyPinHighStoresElided++;
#endif
		return true;
	}

	void BlockCompiler::ClearGprQCache()
	{
		m_gpr_q_cache_count = 0;
	}

	void BlockCompiler::InvalidateGprQCacheForGuest(unsigned guest_reg)
	{
		if (guest_reg == 0 || m_gpr_q_cache_count == 0)
			return;

		for (unsigned i = 0; i < m_gpr_q_cache_count;)
		{
			if (m_gpr_q_cache_guest[i] == guest_reg)
			{
				const unsigned last = --m_gpr_q_cache_count;
				m_gpr_q_cache_guest[i] = m_gpr_q_cache_guest[last];
				m_gpr_q_cache_qreg[i] = m_gpr_q_cache_qreg[last];
				continue;
			}
			i++;
		}
	}

	void BlockCompiler::InvalidateGprQCacheForQreg(unsigned qreg)
	{
		if (m_gpr_q_cache_count == 0)
			return;

		for (unsigned i = 0; i < m_gpr_q_cache_count;)
		{
			if (m_gpr_q_cache_qreg[i] == qreg)
			{
				const unsigned last = --m_gpr_q_cache_count;
				m_gpr_q_cache_guest[i] = m_gpr_q_cache_guest[last];
				m_gpr_q_cache_qreg[i] = m_gpr_q_cache_qreg[last];
				continue;
			}
			i++;
		}
	}

	void BlockCompiler::MarkGprQCache(unsigned guest_reg, unsigned qreg)
	{
		if (!m_gpr_q_cache_enabled || guest_reg == 0 || qreg >= MAX_GPR_QCACHE)
			return;

		InvalidateGprQCacheForGuest(guest_reg);
		InvalidateGprQCacheForQreg(qreg);
		if (m_gpr_q_cache_count >= MAX_GPR_QCACHE)
			return;

		m_gpr_q_cache_guest[m_gpr_q_cache_count] = static_cast<u8>(guest_reg);
		m_gpr_q_cache_qreg[m_gpr_q_cache_count] = static_cast<u8>(qreg);
		m_gpr_q_cache_count++;
	}

	int BlockCompiler::FindGprQCache(unsigned guest_reg) const
	{
		if (!m_gpr_q_cache_enabled || guest_reg == 0)
			return -1;

		for (unsigned i = 0; i < m_gpr_q_cache_count; i++)
		{
			if (m_gpr_q_cache_guest[i] == guest_reg)
				return static_cast<int>(m_gpr_q_cache_qreg[i]);
		}

		return -1;
	}

	bool BlockCompiler::IsGprQCacheQregResident(unsigned qreg) const
	{
		if (!m_gpr_q_cache_enabled)
			return false;

		for (unsigned i = 0; i < m_gpr_q_cache_count; i++)
		{
			if (m_gpr_q_cache_qreg[i] == qreg)
				return true;
		}

		return false;
	}

	bool BlockCompiler::GprQCacheGuestHasFutureQwordReadBeforeWrite(unsigned guest_reg) const
	{
		if (!m_gpr_q_cache_enabled || guest_reg == 0 ||
			m_current_instruction_index + 1 >= m_current_block_instruction_count)
		{
			return false;
		}

		for (u32 i = m_current_instruction_index + 1;
			 i < m_current_block_instruction_count; i++)
		{
			u16 counts[32]{};
			u32 defined = 0;
			CountGprQCacheEntryQwordUses(
				memRead32(m_current_block_start_pc + i * sizeof(u32)), counts, defined);
			if (counts[guest_reg] != 0)
				return true;
			if ((defined & (1u << guest_reg)) != 0)
				return false;
		}

		return false;
	}

	bool BlockCompiler::GprQCacheGuestHasFutureQfsrvSourceReadBeforeWrite(unsigned guest_reg) const
	{
		if (!m_gpr_q_cache_enabled || guest_reg == 0 ||
			m_current_instruction_index + 1 >= m_current_block_instruction_count)
		{
			return false;
		}

		for (u32 i = m_current_instruction_index + 1;
			 i < m_current_block_instruction_count; i++)
		{
			const u32 future_op = memRead32(m_current_block_start_pc + i * sizeof(u32));
			// PCSX2 owner: MMI.cpp::QFSRV(), reached through MMI1 subop 0x1b.
			if ((future_op >> 26) == 0x1c && (future_op & 0x3f) == 0x28 &&
				((future_op >> 6) & 0x1f) == 0x1b)
			{
				if (RS(future_op) == guest_reg || RT(future_op) == guest_reg)
					return true;
				if (RD(future_op) == guest_reg)
					return false;
			}

			u16 counts[32]{};
			u32 defined = 0;
			CountGprQCacheEntryQwordUses(future_op, counts, defined);
			if ((defined & (1u << guest_reg)) != 0)
				return false;
		}

		return false;
	}

	bool BlockCompiler::GprQCacheQregHasFutureQwordReadBeforeWrite(unsigned qreg) const
	{
		if (!m_gpr_q_cache_enabled)
			return false;

		for (unsigned i = 0; i < m_gpr_q_cache_count; i++)
		{
			if (m_gpr_q_cache_qreg[i] == qreg)
				return GprQCacheGuestHasFutureQwordReadBeforeWrite(m_gpr_q_cache_guest[i]);
		}

		return false;
	}

	u32 BlockCompiler::GprQCacheGuestNextQwordReadDistanceBeforeWrite(unsigned guest_reg) const
	{
		if (!m_gpr_q_cache_enabled || guest_reg == 0 ||
			m_gpr_q_cache_next_use_distances.size() !=
				static_cast<size_t>(m_current_block_instruction_count) * 32 ||
			m_current_instruction_index >= m_current_block_instruction_count)
		{
			return UINT32_MAX;
		}

		const u16 distance = m_gpr_q_cache_next_use_distances[
			static_cast<size_t>(m_current_instruction_index) * 32 + guest_reg];
		return distance == UINT16_MAX ? UINT32_MAX : distance;
	}

	u32 BlockCompiler::GprQCacheQregNextQwordReadDistanceBeforeWrite(unsigned qreg) const
	{
		for (unsigned i = 0; i < m_gpr_q_cache_count; i++)
		{
			if (m_gpr_q_cache_qreg[i] == qreg)
			{
				return GprQCacheGuestNextQwordReadDistanceBeforeWrite(
					m_gpr_q_cache_guest[i]);
			}
		}

		return UINT32_MAX;
	}

	unsigned BlockCompiler::SelectGprQCacheScratchQreg(u32 avoid_qreg_mask) const
	{
		unsigned farthest_qreg = MAX_GPR_QCACHE;
		u32 farthest_distance = 0;
		for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
		{
			if ((avoid_qreg_mask & (1u << qreg)) != 0)
				continue;

			if (!IsGprQCacheQregResident(qreg))
				return qreg;

			const u32 distance = GprQCacheQregNextQwordReadDistanceBeforeWrite(qreg);
			if (farthest_qreg == MAX_GPR_QCACHE || distance > farthest_distance)
			{
				farthest_qreg = qreg;
				farthest_distance = distance;
			}
		}

		return farthest_qreg;
	}

	bool BlockCompiler::GprQCacheGuestDefinedBeforeCurrentInstruction(unsigned guest_reg) const
	{
		if (!m_gpr_q_cache_enabled || guest_reg == 0 ||
			m_current_instruction_index == 0)
		{
			return false;
		}

		u32 defined = 1;
		for (u32 i = 0; i < m_current_instruction_index; i++)
		{
			u16 counts[32]{};
			CountGprQCacheEntryQwordUses(
				memRead32(m_current_block_start_pc + i * sizeof(u32)), counts, defined);
		}

		return (defined & (1u << guest_reg)) != 0;
	}

	u16 BlockCompiler::GprQCacheGuestEntryQwordReadCount(unsigned guest_reg) const
	{
		if (!m_gpr_q_cache_enabled || guest_reg == 0)
			return 0;

		u16 counts[32]{};
		u32 defined = 1;
		for (u32 i = 0; i < m_current_block_instruction_count; i++)
		{
			CountGprQCacheEntryQwordUses(
				memRead32(m_current_block_start_pc + i * sizeof(u32)), counts, defined);
		}

		return counts[guest_reg];
	}

	bool BlockCompiler::PreserveGprQCacheGuestForFutureRead(unsigned guest_reg, unsigned cached_qreg,
		unsigned avoid_qreg0, unsigned avoid_qreg1, bool* preserved)
	{
		if (preserved)
			*preserved = false;

		if (!GprQCacheGuestHasFutureQwordReadBeforeWrite(guest_reg))
			return true;

		u32 avoid_qreg_mask = 1u << cached_qreg;
		if (avoid_qreg0 < MAX_GPR_QCACHE)
			avoid_qreg_mask |= 1u << avoid_qreg0;
		if (avoid_qreg1 < MAX_GPR_QCACHE)
			avoid_qreg_mask |= 1u << avoid_qreg1;
		const unsigned qreg = SelectGprQCacheScratchQreg(avoid_qreg_mask);
		if (qreg >= MAX_GPR_QCACHE)
			return true;

		if (IsGprQCacheQregResident(qreg) &&
			GprQCacheGuestNextQwordReadDistanceBeforeWrite(guest_reg) >=
				GprQCacheQregNextQwordReadDistanceBeforeWrite(qreg))
		{
			return true;
		}

		if (!m_code.EmitVorrQ(qreg, cached_qreg, cached_qreg))
			return false;
		MarkGprQCache(guest_reg, qreg);
		if (preserved)
			*preserved = true;
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprQCacheDirectPreservedMutatingSourceCopies++;
#endif
		return true;
	}

	bool BlockCompiler::EmitStoreGprQ128PreservingCachedSourceIfFutureRead(unsigned dest_guest_reg,
		unsigned source_guest_reg, unsigned cached_qreg, unsigned address_scratch, bool* preserved)
	{
		if (preserved)
			*preserved = false;

		if (GprQCacheGuestHasFutureQwordReadBeforeWrite(source_guest_reg))
		{
			const unsigned qreg = SelectGprQCacheScratchQreg(1u << cached_qreg);
			if (qreg < MAX_GPR_QCACHE &&
				(!IsGprQCacheQregResident(qreg) ||
				 GprQCacheGuestNextQwordReadDistanceBeforeWrite(source_guest_reg) <
					 GprQCacheQregNextQwordReadDistanceBeforeWrite(qreg)))
			{
				if (preserved)
					*preserved = true;
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectPreservedCopyStores++;
#endif
				return m_code.EmitVorrQ(qreg, cached_qreg, cached_qreg) &&
					   EmitStoreGprQ128(dest_guest_reg, qreg, address_scratch);
			}
		}

		return EmitStoreGprQ128(dest_guest_reg, cached_qreg, address_scratch);
	}

	bool BlockCompiler::BeginBlock(bool use_vtlb_registers, bool use_cop1_exponent_mask_register,
		bool use_vu0_base_register, size_t* linked_entry_offset,
		u32 linked_entry_pc, bool linked_entry_needs_pc_sync)
	{
		m_scalar_load_cold_tails.clear();
		m_scalar_store_cold_tails.clear();
		m_qword_load_cold_tails.clear();
		m_qword_store_cold_tails.clear();
		m_cop1_word_memory_cold_tails.clear();
		m_cop2_qword_memory_cold_tails.clear();
		m_vu0_sync_cold_tails.clear();
		m_partial_memory_cold_tails.clear();
		m_vtlb_registers_available = use_vtlb_registers;
		m_cop1_exponent_mask_available = use_cop1_exponent_mask_register;
		m_vu0_base_available = use_vu0_base_register;
		ClearGprQCache();
		ClearGprConstState();
		ClearSaConstState();
		ClearCop1NormalizedState();
		m_cop2_norm_consts_ready = false;
		for (unsigned i = 0; i < MAX_GPR_PINS; i++)
		{
			m_pin_dirty_low[i] = false;
			m_pin_dirty_high[i] = false;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprConstBlocks++;
		if (m_gpr_q_cache_enabled)
			g_qemuGprQCacheBlocks++;
#endif
		// Native EE links jump body-to-body under one uniform core-register frame.
		// The private vector ABI maps logical q4-q7 to caller-clobbered q12-q15,
		// so callable entries no longer need a vector stack frame.
		m_saved_registers = EE_LINK_FRAME_REGISTERS;

		// Adopt the staged GPR pins, dropping any whose host register a block
		// feature claimed after staging.
		m_pin_count = 0;
		for (unsigned i = 0; i < m_staged_pin_count; i++)
		{
			const unsigned host = m_staged_pin_host[i];
			const unsigned high_host = m_staged_pin_high_host[i];
			if ((host == HOST_VTLB_VMAP && use_vtlb_registers) ||
				(host == HOST_VTLB_HOST_MEMORY_BASE && use_vtlb_registers) ||
				(host == HOST_COP1_EXPONENT_MASK && use_cop1_exponent_mask_register) ||
				(host == HOST_VU0_BASE && use_vu0_base_register) ||
				(high_host == HOST_VTLB_VMAP && use_vtlb_registers) ||
				(high_host == HOST_VTLB_HOST_MEMORY_BASE && use_vtlb_registers) ||
				(high_host == HOST_COP1_EXPONENT_MASK && use_cop1_exponent_mask_register) ||
				(high_host == HOST_VU0_BASE && use_vu0_base_register))
			{
				continue;
			}

			m_pin_guest[m_pin_count] = m_staged_pin_guest[i];
			m_pin_host[m_pin_count] = static_cast<u8>(host);
			m_pin_high_host[m_pin_count] = static_cast<u8>(high_host);
			m_pin_needs_entry_load[m_pin_count] = m_staged_pin_needs_entry_load[i];
			m_pin_count++;
		}
		m_staged_pin_count = 0;

#if defined(VITASX2_QEMU_VALIDATION)
		if (m_pin_count != 0)
		{
			g_qemuGprPinnedBlocks++;
			g_qemuGprPinnedRegisters += m_pin_count;
			for (unsigned i = 0; i < m_pin_count; i++)
			{
				if (m_pin_high_host[i] != NO_GPR_PIN_HOST)
					g_qemuGprPinnedDwordRegisters++;
				if (m_pin_host[i] == HOST_VTLB_VMAP ||
					m_pin_host[i] == HOST_VTLB_HOST_MEMORY_BASE)
					g_qemuGprPinnedVtlbFreeHostRegisters++;
				if (m_pin_high_host[i] == HOST_VTLB_VMAP ||
					m_pin_high_host[i] == HOST_VTLB_HOST_MEMORY_BASE)
					g_qemuGprPinnedVtlbFreeHostRegisters++;
			}
		}
#endif

		if (m_persistent_dispatch_exits)
		{
			// PCSX2's _DynGen_EnterRecompiledCode() owns one native frame around its
			// non-returning dispatcher. Persistent Vita blocks are entered only under
			// the equivalent dispatcher frame, with r4 already holding &cpuRegs, so
			// they contain no dead callable prologue.
			if (linked_entry_offset)
				*linked_entry_offset = m_code.Size();
#if defined(VITASX2_QEMU_VALIDATION)
			if (linked_entry_needs_pc_sync)
				g_qemuLinkedPcSyncBlocks++;
#endif
			if (linked_entry_needs_pc_sync && !EmitStorePc(linked_entry_pc))
				return false;
		}
		else
		{
			if (!m_code.EmitPush(m_saved_registers | REG_LR) ||
				!m_code.EmitMovImm32(HOST_CPU_REGS, static_cast<u32>(reinterpret_cast<uptr>(&cpuRegs))))
			{
				return false;
			}

			// r4 is never used as a pin/scratch register and AAPCS helpers preserve
			// it, so generated links can skip the callable-entry frame setup and the
			// cpuRegs base materialization while still letting final exits pop once.
			// Callable entry arrives with cpuRegs.pc set by the executor and skips the
			// linked-only synchronization used by helper/exception-observing blocks.
			if (linked_entry_offset && linked_entry_needs_pc_sync)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuLinkedPcSyncBlocks++;
#endif
				const size_t callable_body = m_code.EmitBranchPlaceholder();
				if (callable_body == static_cast<size_t>(-1))
					return false;

				*linked_entry_offset = m_code.Size();
				if (!EmitStorePc(linked_entry_pc) || !m_code.PatchBranch(callable_body, m_code.Size()))
					return false;
			}
			else if (linked_entry_offset)
			{
				*linked_entry_offset = m_code.Size();
			}
		}

		if (use_cop1_exponent_mask_register &&
			!m_code.EmitMovImm32(HOST_COP1_EXPONENT_MASK, FPU_FLOAT_EXPONENT_MASK))
		{
			return false;
		}

		if (use_vu0_base_register)
		{
			// PCSX2 owner: VU0.cpp's singleton VU0 state. This is only worth a
			// callee-saved register when the block really addresses VU0 state.
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuVu0BaseRegisterBlocks++;
#endif
			if (!m_code.EmitMovImm32(HOST_VU0_BASE, static_cast<u32>(reinterpret_cast<uptr>(&VU0))))
				return false;
		}

		// PCSX2 owner: vtlb.cpp::vtlb_memRead*()/vtlb_memWrite*() read these
		// stable pointers for every access. The persistent dispatcher owns r7/r8
		// for its entire private frame, so its blocks inherit them instead of
		// rematerializing both pointers at every linked block entry. Callable
		// blocks still establish the same state locally.
#if defined(VITASX2_QEMU_VALIDATION)
		if (use_vtlb_registers && m_persistent_dispatch_exits)
			g_qemuPersistentVtlbResidentBlocks++;
#endif
		return !use_vtlb_registers || m_persistent_dispatch_exits ||
			   (m_code.EmitMovImm32(HOST_VTLB_VMAP,
				   static_cast<u32>(reinterpret_cast<uptr>(&vtlb_private::vtlbdata.vmap))) &&
			   m_code.EmitLdrImm12(HOST_VTLB_VMAP, HOST_VTLB_VMAP, 0) &&
			   m_code.EmitMovImm32(HOST_VTLB_HOST_MEMORY_BASE,
				   static_cast<u32>(reinterpret_cast<uptr>(&vtlb_private::vtlbdata.host_memory_base))) &&
			   m_code.EmitLdrImm12(HOST_VTLB_HOST_MEMORY_BASE, HOST_VTLB_HOST_MEMORY_BASE, 0));
	}

	bool BlockCompiler::CompileStraightLineBlock(u32 start_pc, u32 instruction_count, const void* direct_exit,
		const void* event_exit, u32* scaled_cycles, DirectLinkSlots* direct_links,
		const void* indirect_lookup_pages_slot, const void* direct_linking_enabled_flag,
		size_t* linked_entry_offset, bool persistent_dispatch_exits,
		size_t* resident_self_link_entry_offset, u8* resident_self_link_entry_loads,
		const GprLinkSignature* gpr_link_signature,
		size_t* compatible_link_entry_offset, u8* compatible_link_entry_loads)
	{
		if (instruction_count == 0 || instruction_count > ((UINT32_MAX - start_pc) / 4))
			return false;

		if (direct_links)
			*direct_links = {};
		if (resident_self_link_entry_offset)
			*resident_self_link_entry_offset = static_cast<size_t>(-1);
		if (resident_self_link_entry_loads)
			*resident_self_link_entry_loads = 0;
		if (compatible_link_entry_offset)
			*compatible_link_entry_offset = static_cast<size_t>(-1);
		if (compatible_link_entry_loads)
			*compatible_link_entry_loads = 0;

		const u32 previous_block_start_pc = m_current_block_start_pc;
		const u32 previous_block_instruction_count = m_current_block_instruction_count;
		const u32 previous_instruction_index = m_current_instruction_index;
		const bool previous_persistent_dispatch_exits = m_persistent_dispatch_exits;
		struct CurrentBlockScope
		{
			BlockCompiler& compiler;
			u32 previous_start_pc;
			u32 previous_instruction_count;
			u32 previous_instruction_index;
			bool previous_persistent_dispatch_exits;
			~CurrentBlockScope()
			{
				compiler.m_current_block_start_pc = previous_start_pc;
				compiler.m_current_block_instruction_count = previous_instruction_count;
				compiler.m_current_instruction_index = previous_instruction_index;
				compiler.m_persistent_dispatch_exits = previous_persistent_dispatch_exits;
			}
		} current_block_scope{
			*this, previous_block_start_pc, previous_block_instruction_count,
			previous_instruction_index, previous_persistent_dispatch_exits};
		m_current_block_start_pc = start_pc;
		m_current_block_instruction_count = instruction_count;
		m_current_instruction_index = 0;
		m_persistent_dispatch_exits = persistent_dispatch_exits;
		m_gpr_link_signature =
			(persistent_dispatch_exits && gpr_link_signature && gpr_link_signature->IsValid()) ?
				*gpr_link_signature : GprLinkSignature{};

		m_reclaimed_vtlb_link_hosts = m_gpr_link_signature.ReclaimsVtlbHosts();
		const bool use_vtlb_registers =
			BlockNeedsResidentVtlbRegisters(start_pc, instruction_count) &&
			!m_reclaimed_vtlb_link_hosts;
		const bool use_cop1_exponent_mask_register =
			BlockShouldUseCop1ExponentMaskRegister(start_pc, instruction_count);
		const bool use_vu0_base_register = BlockShouldUseVu0BaseRegister(start_pc, instruction_count);
		const bool linked_entry_needs_pc_sync = BlockNeedsLinkedPcSync(start_pc, instruction_count);
		const bool dirty_pins_candidate = BlockCanUseDirtyGprPins(start_pc, instruction_count);
		const bool dirty_self_link_shape_candidate = persistent_dispatch_exits && direct_links &&
			dirty_pins_candidate && BlockHasExactConditionalSelfLink(start_pc, instruction_count);
		const bool caller_saved_branch_flag = dirty_self_link_shape_candidate &&
			BlockCanUseCallerSavedBranchFlag(start_pc, instruction_count);
		m_branch_flag_host = caller_saved_branch_flag ?
			HOST_CALLER_SAVED_BRANCH_FLAG : HOST_BRANCH_FLAG;
#if defined(VITASX2_QEMU_VALIDATION)
		if (caller_saved_branch_flag)
			g_qemuCallerSavedBranchFlagBlocks++;
#endif
		const bool persistent_vtlb_registers =
			persistent_dispatch_exits && !m_reclaimed_vtlb_link_hosts;
		m_dirty_pins_enabled = false;
		m_gpr_q_cache_enabled = BlockShouldUseGprQCache(start_pc, instruction_count);
		StageGprQCacheForBlock(start_pc, instruction_count);
		// A staged qcache reload occurs after the resident scalar-pin entry. Until
		// exact self-links can carry or synchronize overlapping qregs, keep their
		// backing state authoritative through the ordinary exit flush. Merely using
		// the qcache within a block is safe when it has no entry reloads (for example,
		// SQ zero in the measured zero-fill loop).
		const bool dirty_self_link_candidate = dirty_self_link_shape_candidate &&
			m_staged_gpr_q_cache_count == 0;
		m_forwarded_boolean_branch = dirty_self_link_candidate && caller_saved_branch_flag &&
			AnalyzeForwardedBooleanBranch(start_pc, instruction_count,
				&m_forwarded_boolean_guest, &m_forwarded_boolean_producer_index);
		if (!m_forwarded_boolean_branch)
		{
			m_forwarded_boolean_guest = 0;
			m_forwarded_boolean_producer_index = 0;
		}
		m_resident_raw_gpr0_qword = m_forwarded_boolean_branch &&
			ForwardedBooleanPrefixStoresRawGpr0(
				start_pc, m_forwarded_boolean_producer_index);
		m_resident_raw_gpr0_entry_instructions = 0;
		m_resident_vtlb_qword_store_op = 0;
		const u32 forwarded_producer_op = m_forwarded_boolean_branch ?
			memRead32(start_pc + m_forwarded_boolean_producer_index * sizeof(u32)) : 0;
		const bool forwarded_producer_reads_old_result = m_forwarded_boolean_branch &&
			(RS(forwarded_producer_op) == m_forwarded_boolean_guest ||
			 RT(forwarded_producer_op) == m_forwarded_boolean_guest);
		m_resident_vtlb_qword_pointer = m_resident_raw_gpr0_qword &&
			!forwarded_producer_reads_old_result &&
			AnalyzeResidentSequentialRawGpr0QwordStore(start_pc, instruction_count,
				m_forwarded_boolean_producer_index, &m_resident_vtlb_qword_store_op);
		m_resident_forwarded_boolean_mask = m_resident_vtlb_qword_pointer;
		m_resident_cycle_low = m_resident_vtlb_qword_pointer;
		m_resident_scheduler_countdown = m_resident_cycle_low;
		m_compatible_scheduler_countdown =
			m_gpr_link_signature.HasSchedulerCountdown();
		m_compatible_vtlb_pointer = m_gpr_link_signature.HasVtlbPointer();
		m_compatible_vtlb_pointer_access = m_compatible_vtlb_pointer &&
			start_pc == m_gpr_link_signature.vtlb_pointer.access_pc;
		m_compatible_vtlb_pointer_unaligned_fallback = static_cast<size_t>(-1);
		m_compatible_vtlb_pointer_handler_fallback = static_cast<size_t>(-1);
		m_compatible_vtlb_pointer_dirty_pins = {};
		m_compatible_vtlb_pointer_guard_instructions = 0;
		m_compatible_vtlb_pointer_translation_instructions = 0;
		m_deferred_resident_unsigned_branch_suffix =
			m_resident_scheduler_countdown && (forwarded_producer_op & 0x3f) == 0x2b &&
			RS(forwarded_producer_op) != 0 && RT(forwarded_producer_op) != 0 &&
			RS(forwarded_producer_op) != RT(forwarded_producer_op);
		m_deferred_resident_unsigned_compare_op =
			m_deferred_resident_unsigned_branch_suffix ? forwarded_producer_op : 0;
		m_deferred_resident_delay_op = m_deferred_resident_unsigned_branch_suffix ?
			memRead32(start_pc + (instruction_count - 1) * sizeof(u32)) : 0;
		m_resident_vtlb_qword_guard_offset = static_cast<size_t>(-1);
		m_resident_vtlb_qword_handler_fallback = static_cast<size_t>(-1);
		m_resident_vtlb_qword_dirty_pins = {};
		m_resident_vtlb_qword_guard_instructions = 0;
		m_resident_vtlb_qword_translation_instructions = 0;
#if defined(VITASX2_QEMU_VALIDATION)
		if (m_forwarded_boolean_branch)
			g_qemuForwardedBooleanBranchBlocks++;
		if (m_deferred_resident_unsigned_branch_suffix)
			g_qemuResidentUnsignedBranchSuffixBlocks++;
#endif
		// r7/r8 are normally a chain-wide vTLB ABI under the persistent dispatcher.
		// A compatible translated-pointer signature may reclaim them because its
		// cold translator materializes both bases and every external exit restores
		// the dispatcher contract before another block can observe the registers.
		StageGprPinsForBlock(start_pc, instruction_count,
			caller_saved_branch_flag,
			!use_vtlb_registers && !persistent_vtlb_registers,
			!use_vtlb_registers && !persistent_vtlb_registers,
			!use_cop1_exponent_mask_register, !use_vu0_base_register, dirty_pins_candidate,
			dirty_self_link_candidate);
		if (m_gpr_link_signature.IsValid())
			StageGprPinsForLinkSignature(start_pc, instruction_count, m_gpr_link_signature);
		if (linked_entry_offset)
			*linked_entry_offset = 0;

		if (!BeginBlock(use_vtlb_registers, use_cop1_exponent_mask_register, use_vu0_base_register,
				linked_entry_offset, start_pc,
				linked_entry_needs_pc_sync && !m_gpr_link_signature.IsValid()))
		{
			return false;
		}
		if (m_gpr_link_signature.IsValid() && m_pin_count != m_gpr_link_signature.count)
			return false;
		m_dirty_pins_enabled = dirty_pins_candidate &&
			(m_gpr_link_signature.IsValid() ? m_gpr_link_signature.HasWriteBack() :
				BlockWritesPinnedGpr(start_pc, instruction_count));
#if defined(VITASX2_QEMU_VALIDATION)
		if (m_dirty_pins_enabled)
			g_qemuGprDirtyPinBlocks++;
#endif
		// Pins must be live before any emitted GPR read, including the Goemon
		// hook's GPR4 argument load; the hook's helpers are AAPCS calls that
		// preserve the callee-saved pin hosts and never write the GPR file.
		const u8 gpr_pin_entry_loads = GprPinEntryLoadInstructionCount();
		const bool preserve_dirty_self_link = dirty_self_link_candidate &&
			m_dirty_pins_enabled && gpr_pin_entry_loads != 0;
		if (preserve_dirty_self_link)
			MarkGprPinsDirtyAtResidentSelfLinkEntry(start_pc, instruction_count);
		if (!EmitGprPinLoads())
			return false;
		if (m_dirty_pins_enabled && m_gpr_link_signature.IsValid())
		{
			for (u8 i = 0; i < m_gpr_link_signature.count; i++)
			{
				const GprLinkMapping& mapping = m_gpr_link_signature.mappings[i];
				if (mapping.dirty != GprLinkDirtyState::WriteBack)
					continue;
				m_pin_dirty_low[i] = true;
				m_pin_dirty_high[i] = mapping.width == GprLinkWidth::Low64;
			}
		}
		if (!EmitStageCompatibleSchedulerCountdown(HOST_TMP0))
			return false;
		if (!EmitPoisonCompatibleVtlbPointer())
			return false;
		if (m_gpr_link_signature.IsValid())
		{
			if (compatible_link_entry_offset)
				*compatible_link_entry_offset = m_code.Size();
			if (compatible_link_entry_loads)
				*compatible_link_entry_loads = gpr_pin_entry_loads;
			if (linked_entry_needs_pc_sync)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuLinkedPcSyncBlocks++;
#endif
				if (!EmitStorePc(start_pc))
					return false;
			}
		}
		if (!EmitStageCompatibleVtlbPointer())
			return false;
		u8 resident_entry_loads = gpr_pin_entry_loads;
		if (m_forwarded_boolean_branch)
		{
			const size_t offset = GprOffset(m_forwarded_boolean_guest);
			if (!m_code.EmitLdrImm12(m_branch_flag_host, HOST_CPU_REGS, static_cast<u16>(offset)) ||
				!m_code.EmitLdrImm12(HOST_TMP5, HOST_CPU_REGS,
					static_cast<u16>(offset + sizeof(u32))))
			{
				return false;
			}
			resident_entry_loads += 2;
		}
		if (!EmitStageResidentRawGpr0Qword())
			return false;
		if (!EmitStageResidentSchedulerCountdown(true))
			return false;
		if (!EmitStageResidentVtlbQwordPointer())
			return false;
		if (persistent_dispatch_exits && resident_entry_loads != 0)
		{
			// PCSX2 owner: iCore.cpp keeps MODE_READ mappings valid until a
			// clobber/flush seam, while iBranchTest()/BaseBlocks owns the patched
			// direct edge. A self-link returns to this exact allocator mapping, so
			// it may enter after scalar/forwarded/qword staging; the sequential-SQ
			// form enters its translated-pointer advance/guard. Other incoming edges
			// retain the ordinary linked entry and rebuild the mapping from cpuRegs.
			// The self edge also cannot inherit a different PC: normal entry has
			// already synchronized this same block-start PC, and exception/event
			// paths leave through the dispatcher rather than taking the self edge.
			if (resident_self_link_entry_offset)
			{
				*resident_self_link_entry_offset = m_resident_vtlb_qword_pointer ?
					m_resident_vtlb_qword_guard_offset : m_code.Size();
			}
			if (resident_self_link_entry_loads)
				*resident_self_link_entry_loads = resident_entry_loads;
		}
		if (!EmitGprQCacheEntryLoads())
			return false;
		if (!EmitGoemonBlockStartHook(start_pc))
			return false;

		u32 raw_cycles = 0;
		u32 committed_scaled_cycles = 0;
		bool has_branch = false;
		bool has_register_branch_target = false;
		bool has_static_direct_link_target = false;
		bool has_static_register_branch_target = false;
		bool has_static_conditional_direct_links = false;
		bool has_static_likely_direct_links = false;
		bool branch_is_likely = false;
		bool branch_likely_delay_slot_cancelled = false;
		bool pending_di_clear = false;
		u32 branch_instruction_index = 0;
		u32 branch_target_pc = 0;
		u32 static_direct_link_target_pc = 0;
		u32 branch_likely_not_taken_raw_cycles = 0;
		size_t branch_likely_skip_delay = static_cast<size_t>(-1);
		const auto set_static_branch_link = [&](u32 target_pc) {
			has_static_direct_link_target = true;
			static_direct_link_target_pc = target_pc;
		};
		const auto try_emit_static_register_branch = [&](u32 branch_op, u32 branch_pc, bool link,
			bool* handled) {
			u32 known_target_pc = 0;
			if (!TryGetKnownGprLow(RS(branch_op), &known_target_pc))
			{
				*handled = false;
				return true;
			}

			// PCSX2 owners: Interpreter.cpp::JR()/JALR() and
			// x86/ix86-32/iR5900Jump.cpp::recJR()/recJALR(). When the low
			// target word is block-known, snapshot it here and use the same
			// static direct-link tail as J/JAL instead of burning an indirect
			// register-lookup exit on Cortex-A9.
			if (EmuConfig.Gamefixes.GoemonTlbHack)
				known_target_pc = vtlb_V2P(known_target_pc);
			branch_target_pc = known_target_pc;
			set_static_branch_link(known_target_pc);
			has_static_register_branch_target = true;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprConstRegisterJumpTargets++;
#endif

			const unsigned rd = RD(branch_op);
			if (link && rd != 0 && !EmitLink(rd, branch_pc))
				return false;

			*handled = true;
			return true;
		};
		const auto add_raw_cycles = [&raw_cycles](u32 op) {
			// PCSX2's x86 recRecompile() gives NOP a fixed 9-cycle raw cost before
			// scaling; all other op costs come from the R5900 opcode table.
			if (op == 0)
				raw_cycles += 9 * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));
			else
				raw_cycles += R5900::GetInstruction(op).cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));
		};

		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 pc = start_pc + i * 4;
			const u32 op = memRead32(pc);

			if (has_branch && i > branch_instruction_index + 1)
				return false;

			if (branch_likely_delay_slot_cancelled && i == branch_instruction_index + 1)
				continue;

			if (IsSupportedBranchOpcode(op))
			{
				if (pending_di_clear)
					return false;

				if (has_branch && i == branch_instruction_index + 1)
	{
					// PCSX2 owner: x86/ix86-32/iR5900.cpp::recompileNextInstruction()
					// detects a branch while compiling the outer branch delay slot,
					// advances PC past it, and emits no side effects or cycles for
					// the delay-slot branch itself.
					if (branch_is_likely &&
						!m_code.PatchBranch(branch_likely_skip_delay, m_code.Size(), VitaA32::Condition::EQ))
	{
						return false;
	}

					continue;
	}

				if (has_branch || i + 1 >= instruction_count)
					return false;

				const u32 delay_op = memRead32(pc + 4);
				if (!CanCompileDelaySlotOpcode(delay_op))
					return false;

				add_raw_cycles(op);
				if (!EmitDeviceTracePreInstruction(pc))
					return false;
				branch_instruction_index = i;
				has_branch = true;
				branch_is_likely = IsBranchLikelyOpcode(op);
				if (branch_is_likely)
					branch_likely_not_taken_raw_cycles = raw_cycles;

				switch (op >> 26)
	{
					case 0x00:
						switch (op & 0x3f)
						{
							case 0x08:
							{
								bool handled = false;
								if (!try_emit_static_register_branch(op, pc, false, &handled))
									return false;
								if (handled)
									break;
								has_register_branch_target = true;
								if (!EmitJR(op, pc))
									return false;
								break;
							}
							case 0x09:
							{
								bool handled = false;
								if (!try_emit_static_register_branch(op, pc, true, &handled))
									return false;
								if (handled)
									break;
								has_register_branch_target = true;
								if (!EmitJALR(op, pc))
									return false;
								break;
							}
							default:
								return false;
						}
						break;
					case 0x01:
						branch_target_pc = BranchTarget(pc, op);
						{
							bool constant_branch_taken = false;
							if (TryEvaluateConstantRegimmLinkBranch(op, &constant_branch_taken))
							{
								if (!EmitLink(31, pc))
									return false;
								set_static_branch_link(constant_branch_taken ? branch_target_pc : pc + 8);
								if (branch_is_likely)
								{
									branch_likely_delay_slot_cancelled = !constant_branch_taken;
									branch_is_likely = false;
								}
								break;
							}
						}
						if (branch_is_likely)
						{
							bool constant_branch_taken = false;
							if (TryEvaluateConstantBranch(op, &constant_branch_taken))
							{
								set_static_branch_link(constant_branch_taken ? branch_target_pc : pc + 8);
								branch_likely_delay_slot_cancelled = !constant_branch_taken;
								branch_is_likely = false;
								break;
							}
							has_static_likely_direct_links = true;
						}
						else
						{
							bool constant_branch_taken = false;
							if (TryEvaluateConstantBranch(op, &constant_branch_taken))
							{
								set_static_branch_link(constant_branch_taken ? branch_target_pc : pc + 8);
								break;
							}
							has_static_conditional_direct_links = true;
						}
						if (!EmitREGIMM(op, pc))
							return false;
						break;
					case 0x02:
						branch_target_pc = JumpTarget(pc, op);
						if (EmuConfig.Gamefixes.GoemonTlbHack)
							branch_target_pc = vtlb_V2P(branch_target_pc);
						set_static_branch_link(branch_target_pc);
						if (!EmitJ(op, pc))
							return false;
						break;
					case 0x03:
						branch_target_pc = JumpTarget(pc, op);
						if (EmuConfig.Gamefixes.GoemonTlbHack)
							branch_target_pc = vtlb_V2P(branch_target_pc);
						set_static_branch_link(branch_target_pc);
						if (!EmitJAL(op, pc))
							return false;
						break;
					case 0x04:
	{
						branch_target_pc = BranchTarget(pc, op);
						bool constant_branch_taken = false;
						if (TryEvaluateConstantBranch(op, &constant_branch_taken))
						{
							set_static_branch_link(constant_branch_taken ? branch_target_pc : pc + 8);
							break;
						}
						has_static_conditional_direct_links = true;
						if (!EmitBEQ(op))
							return false;
						break;
	}
					case 0x05:
	{
						branch_target_pc = BranchTarget(pc, op);
						bool constant_branch_taken = false;
						if (TryEvaluateConstantBranch(op, &constant_branch_taken))
						{
							set_static_branch_link(constant_branch_taken ? branch_target_pc : pc + 8);
							break;
						}
						has_static_conditional_direct_links = true;
						if (!EmitBNE(op))
							return false;
						break;
	}
					case 0x06:
	{
						branch_target_pc = BranchTarget(pc, op);
						bool constant_branch_taken = false;
						if (TryEvaluateConstantBranch(op, &constant_branch_taken))
						{
							set_static_branch_link(constant_branch_taken ? branch_target_pc : pc + 8);
							break;
						}
						has_static_conditional_direct_links = true;
						if (!EmitBLEZ(op))
							return false;
						break;
	}
					case 0x07:
	{
						branch_target_pc = BranchTarget(pc, op);
						bool constant_branch_taken = false;
						if (TryEvaluateConstantBranch(op, &constant_branch_taken))
						{
							set_static_branch_link(constant_branch_taken ? branch_target_pc : pc + 8);
							break;
						}
						has_static_conditional_direct_links = true;
						if (!EmitBGTZ(op))
							return false;
						break;
	}
					case 0x10:
						branch_target_pc = BranchTarget(pc, op);
						if (branch_is_likely)
							has_static_likely_direct_links = true;
						else
							has_static_conditional_direct_links = true;
						if (!EmitCop0Branch(op))
							return false;
						break;
					case 0x11:
						branch_target_pc = BranchTarget(pc, op);
						if (branch_is_likely)
							has_static_likely_direct_links = true;
						else
							has_static_conditional_direct_links = true;
						if (!EmitCop1Branch(op))
							return false;
						break;
					case 0x12:
						branch_target_pc = BranchTarget(pc, op);
						if (branch_is_likely)
							has_static_likely_direct_links = true;
						else
							has_static_conditional_direct_links = true;
						if (!EmitCop2Branch(op))
							return false;
						break;
					case 0x14:
	{
						branch_target_pc = BranchTarget(pc, op);
						bool constant_branch_taken = false;
						if (TryEvaluateConstantBranch(op, &constant_branch_taken))
						{
							set_static_branch_link(constant_branch_taken ? branch_target_pc : pc + 8);
							branch_likely_delay_slot_cancelled = !constant_branch_taken;
							branch_is_likely = false;
							break;
						}
						has_static_likely_direct_links = true;
						if (!EmitBEQL(op))
							return false;
						break;
	}
					case 0x15:
	{
						branch_target_pc = BranchTarget(pc, op);
						bool constant_branch_taken = false;
						if (TryEvaluateConstantBranch(op, &constant_branch_taken))
						{
							set_static_branch_link(constant_branch_taken ? branch_target_pc : pc + 8);
							branch_likely_delay_slot_cancelled = !constant_branch_taken;
							branch_is_likely = false;
							break;
						}
						has_static_likely_direct_links = true;
						if (!EmitBNEL(op))
							return false;
						break;
	}
					case 0x16:
	{
						branch_target_pc = BranchTarget(pc, op);
						bool constant_branch_taken = false;
						if (TryEvaluateConstantBranch(op, &constant_branch_taken))
						{
							set_static_branch_link(constant_branch_taken ? branch_target_pc : pc + 8);
							branch_likely_delay_slot_cancelled = !constant_branch_taken;
							branch_is_likely = false;
							break;
						}
						has_static_likely_direct_links = true;
						if (!EmitBLEZL(op))
							return false;
						break;
	}
					case 0x17:
	{
						branch_target_pc = BranchTarget(pc, op);
						bool constant_branch_taken = false;
						if (TryEvaluateConstantBranch(op, &constant_branch_taken))
						{
							set_static_branch_link(constant_branch_taken ? branch_target_pc : pc + 8);
							branch_likely_delay_slot_cancelled = !constant_branch_taken;
							branch_is_likely = false;
							break;
						}
						has_static_likely_direct_links = true;
						if (!EmitBGTZL(op))
							return false;
						break;
	}
					default:
						return false;
	}

				UpdateGprConstStateAfterOpcode(op, pc);
				UpdateCop1NormalizedStateAfterOpcode(op);
				if (branch_is_likely)
	{
					// PCSX2 owners: Interpreter.cpp::BEQL()/BNEL()/BLEZL()/BGTZL()
					// and REGIMM likely forms cancel the delay slot when the
					// condition is false; x86/ix86-32/iR5900Branch.cpp emits a
					// separate not-taken path without recompileNextInstruction().
					if (!m_code.EmitCmpImm32(m_branch_flag_host, 0))
	{
						return false;
	}

					branch_likely_skip_delay = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
					if (branch_likely_skip_delay == static_cast<size_t>(-1))
						return false;
	}

				continue;
			}

			if ((has_branch && i != branch_instruction_index + 1) || !CanCompileOpcode(op))
				return false;
			if (RequiresBlockEndAfterOpcode(op) && i + 1 != instruction_count)
				return false;

			m_current_instruction_index = i;
			add_raw_cycles(op);
			const bool branch_delay_slot = has_branch && i == branch_instruction_index + 1;
			if (!EmitDeviceTracePreInstruction(pc))
				return false;
			if (IsDI(op))
			{
				// PCSX2 owner: x86/iCOP0.cpp::recDI() compiles the following
				// instruction first, then clears Status.EIE inline. Keep the
				// narrow native path to straight-line cases where the next
				// instruction is definitely emitted by this block.
				if (branch_delay_slot || pending_di_clear || i + 1 >= instruction_count)
					return false;

				const u32 next_op = memRead32(pc + 4);
				// Counter-read loads stay rejected here: their handler cold
				// tail can event-exit at pc + 4 before the delayed Status.EIE
				// clear below would run.
				if (IsSupportedBranchOpcode(next_op) ||
					(RequiresBlockEndAfterOpcode(next_op) && !IsSYNC(next_op)) ||
					IsCounterReadLoad(next_op) ||
					IsCycleCommittingFastCOP0(next_op))
	{
					return false;
	}

				pending_di_clear = true;
				continue;
			}
			const bool defer_resident_suffix_instruction =
				m_deferred_resident_unsigned_branch_suffix &&
				(i == m_forwarded_boolean_producer_index || i + 1 == instruction_count);
			if (!defer_resident_suffix_instruction &&
				!EmitOpcode(op, pc, raw_cycles, event_exit, branch_delay_slot))
				return false;
			UpdateGprConstStateAfterOpcode(op, pc);
			UpdateCop1NormalizedStateAfterOpcode(op);

			if (pending_di_clear)
			{
				if (!EmitDIDelayedStatusClear())
					return false;
				pending_di_clear = false;
			}

			if (IsCycleCommittingFastCOP0(op))
			{
				const u32 committed = ScaleBlockCycles(raw_cycles);
				committed_scaled_cycles += committed;
				raw_cycles = RawCycleRemainderAfterClear(raw_cycles);
			}

			if ((IsSYSCALL(op) || IsBREAK(op) || IsTrapOpcode(op)) && !branch_delay_slot)
			{
				if (scaled_cycles)
					*scaled_cycles = committed_scaled_cycles + ScaleBlockCycles(raw_cycles);
				return FlushColdTails();
			}

			if ((op >> 26) == 0x10 && CanCompileCOP0(op) && !IsFastMFC0(op) && !IsFastMTC0(op) &&
				!IsInBlockTLBReadProbe(op))
			{
				if (scaled_cycles)
					*scaled_cycles = committed_scaled_cycles + ScaleBlockCycles(raw_cycles);
				return FlushColdTails();
			}

			if ((op >> 26) == 0x11 && CanCompileCOP1(op) && !IsFastCOP1InBlock(op))
			{
				if (scaled_cycles)
					*scaled_cycles = committed_scaled_cycles + ScaleBlockCycles(raw_cycles);
				return FlushColdTails();
			}

			if ((op >> 26) == 0x12 && CanCompileCOP2(op) && !IsCOP2BranchOpcode(op) &&
				!IsFastCOP2InBlock(op))
			{
				if (scaled_cycles)
					*scaled_cycles = committed_scaled_cycles + ScaleBlockCycles(raw_cycles);
				return FlushColdTails();
			}

			if (has_branch && branch_is_likely && i == branch_instruction_index + 1)
			{
				if (!m_code.PatchBranch(branch_likely_skip_delay, m_code.Size(), VitaA32::Condition::EQ))
					return false;
			}
		}

		if (pending_di_clear)
			return false;

		const u32 next_pc = start_pc + instruction_count * 4;
		const u32 block_cycles = ScaleBlockCycles(raw_cycles);
		const u32 branch_likely_not_taken_cycles = ScaleBlockCycles(branch_likely_not_taken_raw_cycles);
		if (scaled_cycles)
			*scaled_cycles = committed_scaled_cycles + block_cycles;

		// PCSX2 owners: x86/ix86-32/iR5900.cpp::recRecompile() StartRecomp
		// s_nBlockFF analysis plus iBranchTest()'s WaitLoop fast-forward.
		// A backwards loop branch whose guest body repeats identically until an
		// event runs charges this block's cycles, jumps cpuRegs.cycle to
		// nextEventCycle, and exits through the event tail instead of spinning
		// natively. The loop head may sit in earlier A32 blocks (counter-read
		// loads split blocks here, unlike PCSX2's x86 blocks), so the scan
		// covers [taken target, block end). Goemon's physical direct-link
		// targets are excluded so the virtual target compare stays exact. Known
		// register targets still direct-link, but stay out of this J/JAL/branch
		// fast-forward path unless PCSX2's SetBranchReg timing is proven equal.
		const bool wait_loop_body = EmuConfig.Speedhacks.WaitLoop &&
			!EmuConfig.Gamefixes.GoemonTlbHack &&
			has_branch && !has_register_branch_target &&
			!has_static_register_branch_target &&
			branch_instruction_index + 2 == instruction_count &&
			branch_target_pc <= start_pc &&
			IsWaitLoopBody(branch_target_pc, next_pc,
				start_pc + branch_instruction_index * 4);
		const bool whole_wait_loop_fast_forward = has_branch && has_static_direct_link_target &&
			static_direct_link_target_pc == branch_target_pc && wait_loop_body;
		const bool can_direct_link = !has_branch || has_static_direct_link_target ||
			has_static_conditional_direct_links;
		const bool defer_indirect_pc_writeback = has_register_branch_target &&
			indirect_lookup_pages_slot && direct_linking_enabled_flag;
		const bool defer_pc_writeback = defer_indirect_pc_writeback ||
			(direct_links && !whole_wait_loop_fast_forward &&
				(branch_is_likely ? has_static_likely_direct_links : can_direct_link));
		const u32 direct_pc = has_static_direct_link_target ? static_direct_link_target_pc : next_pc;

#if defined(VITASX2_QEMU_VALIDATION)
		if (defer_pc_writeback)
			g_qemuDeferredPcWritebackBlocks++;
		if (defer_indirect_pc_writeback)
			g_qemuDeferredIndirectPcWritebackBlocks++;
#endif

		// Matches the fall-through/branch writeback in x86/ix86-32/iR5900.cpp,
		// after compiling either a non-branching block or a branch plus delay slot.
		// Native direct links keep this state virtual across helper-free block
		// bodies; their event and unlinked tails materialize the selected PC.
		if (!defer_pc_writeback && has_branch)
		{
			if (has_register_branch_target)
			{
				if (!EmitStorePcFromHostReg(HOST_BRANCH_TARGET))
					return false;
			}
			else if (has_static_direct_link_target)
			{
				if (!EmitStorePc(static_direct_link_target_pc))
					return false;
			}
			else if (!EmitStoreBranchPc(branch_target_pc, next_pc))
			{
				return false;
			}
		}
		else if (!defer_pc_writeback && !EmitStorePc(next_pc))
		{
			return false;
		}

		// A statically-taken loop branch (J/JAL back to the loop head, or a
		// collapsed constant branch whose taken target is the loop head)
		// fast-forwards the whole exit; PCSX2's SetBranchImm(s_branchTo) tail
		// does the same. A collapsed known-not-taken branch links to pc + 8
		// instead of the taken target, so it never matches here.
		if (whole_wait_loop_fast_forward)
		{
			return EndBlockWithWaitLoopFastForward(block_cycles, event_exit) && FlushColdTails();
		}

		if (has_branch && branch_is_likely)
		{
			const bool wait_loop_taken = wait_loop_body && has_static_likely_direct_links;
			DirectLinkSlot* const not_taken_link =
				(direct_links && has_static_likely_direct_links) ? &direct_links->slots[0] : nullptr;
			DirectLinkSlot* const taken_link =
				(direct_links && has_static_likely_direct_links && !wait_loop_taken) ?
					&direct_links->slots[1] : nullptr;
			const bool preserve_dirty_not_taken_link =
				(m_dirty_pins_enabled || m_compatible_scheduler_countdown ||
				 m_compatible_vtlb_pointer) &&
				m_gpr_link_signature.ContainsPc(next_pc);
			const bool preserve_dirty_taken_link =
				(m_dirty_pins_enabled || m_compatible_scheduler_countdown ||
				 m_compatible_vtlb_pointer) &&
				m_gpr_link_signature.ContainsPc(branch_target_pc);
			if (!EndBlockWithLikelyCycleTest(block_cycles, branch_likely_not_taken_cycles, direct_exit, event_exit,
					not_taken_link, taken_link, wait_loop_taken,
					defer_pc_writeback, next_pc, branch_target_pc,
					preserve_dirty_not_taken_link, preserve_dirty_taken_link))
			{
				return false;
			}

			if (!FlushColdTails())
				return false;

			if (direct_links && has_static_likely_direct_links)
			{
				direct_links->slots[0].target_pc = next_pc;
				direct_links->slots[0].valid = true;

				if (!wait_loop_taken)
	{
					direct_links->slots[1].target_pc = branch_target_pc;
					direct_links->slots[1].valid = true;
	}
			}

			return true;
		}

		const bool wait_loop_taken = wait_loop_body && has_static_conditional_direct_links;
		DirectLinkSlot* const direct_link =
			(direct_links && can_direct_link) ? &direct_links->slots[0] : nullptr;
		DirectLinkSlot* const taken_link =
			(direct_links && has_static_conditional_direct_links && !wait_loop_taken) ?
				&direct_links->slots[1] : nullptr;
		const u32 direct_link_pc = has_static_direct_link_target ?
			static_direct_link_target_pc : next_pc;
		const bool preserve_dirty_direct_link =
			(m_dirty_pins_enabled || m_compatible_scheduler_countdown ||
			 m_compatible_vtlb_pointer) &&
			m_gpr_link_signature.ContainsPc(direct_link_pc);
		const bool preserve_dirty_taken_link = preserve_dirty_self_link ||
			((m_dirty_pins_enabled || m_compatible_scheduler_countdown ||
			  m_compatible_vtlb_pointer) &&
			 m_gpr_link_signature.ContainsPc(branch_target_pc));
		if (!EndBlockWithCycleTest(block_cycles, direct_exit, event_exit,
				direct_link, taken_link,
				has_register_branch_target ? indirect_lookup_pages_slot : nullptr,
				has_register_branch_target ? direct_linking_enabled_flag : nullptr,
				wait_loop_taken, defer_pc_writeback,
				direct_pc, branch_target_pc, has_static_conditional_direct_links,
				defer_indirect_pc_writeback, preserve_dirty_direct_link,
				preserve_dirty_taken_link))
		{
			return false;
		}

		if (!FlushColdTails())
			return false;

		if (direct_links && can_direct_link)
		{
			direct_links->slots[0].target_pc = has_static_direct_link_target ? static_direct_link_target_pc : next_pc;
			direct_links->slots[0].valid = true;

			if (has_static_conditional_direct_links && !wait_loop_taken)
			{
				direct_links->slots[1].target_pc = branch_target_pc;
				direct_links->slots[1].valid = true;
			}
		}

		return true;
	}

	bool BlockCompiler::EmitOpcode(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		const u32 previous_opcode = m_current_opcode;
		m_current_opcode = op;
		struct CurrentOpcodeScope
		{
			BlockCompiler& compiler;
			u32 previous;
			~CurrentOpcodeScope()
			{
				compiler.m_current_opcode = previous;
			}
		} current_opcode_scope{*this, previous_opcode};

		switch (op >> 26)
		{
			case 0x00:
				return EmitSPECIAL(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x01: // REGIMM, including traps and MTSAB/MTSAH from R5900OpcodeImpl.cpp.
				if (IsRegImmTrap(op))
					return EmitTrapEventExit(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
				return EmitREGIMM(op, pc);
			case 0x08: // ADDI, owned by R5900OpcodeImpl.cpp::ADDI(). The PCSX2
				// recompiler owner x86/ix86-32/iR5900AritImm.cpp::recADDI_() drops
				// the integer-overflow exception, so ADDI compiles exactly like ADDIU.
				return EmitADDIU(op);
			case 0x09: // ADDIU, owned by R5900OpcodeImpl.cpp::ADDIU().
				return EmitADDIU(op);
			case 0x0a: // SLTI, owned by R5900OpcodeImpl.cpp::SLTI().
				return EmitSLTI(op);
			case 0x0b: // SLTIU, owned by R5900OpcodeImpl.cpp::SLTIU().
				return EmitSLTIU(op);
			case 0x0c: // ANDI, owned by R5900OpcodeImpl.cpp::ANDI().
				return EmitANDI(op);
			case 0x0d: // ORI, owned by R5900OpcodeImpl.cpp::ORI().
				return EmitORI(op);
			case 0x0e: // XORI, owned by R5900OpcodeImpl.cpp::XORI().
				return EmitXORI(op);
			case 0x0f: // LUI, owned by R5900OpcodeImpl.cpp::LUI().
				return EmitLUI(op);
			case 0x10: // COP0 helper-backed system ops, owned by COP0.cpp and x86/iCOP0.cpp.
				return EmitCOP0(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x11: // COP1 helper-backed scalar/control ops, owned by FPU.cpp and x86/iFPU.cpp.
				return EmitCOP1(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x12: // COP2/VU0 macro interface, owned by COP2.cpp, VU0.cpp, and x86/microVU_Macro.inl.
				return EmitCOP2(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x18: // DADDI, owned by R5900OpcodeImpl.cpp::DADDI(); overflow trap
				// dropped by x86/ix86-32/iR5900AritImm.cpp::recDADDI(), compiled as DADDIU.
				return EmitDADDIU(op);
			case 0x19: // DADDIU, owned by R5900OpcodeImpl.cpp::DADDIU().
				return EmitDADDIU(op);
			case 0x1a: // LDL, owned by R5900OpcodeImpl.cpp::LDL().
				return EmitLDL(op);
			case 0x1b: // LDR, owned by R5900OpcodeImpl.cpp::LDR().
				return EmitLDR(op);
			case 0x1c: // MMI scalar mult/div extensions, owned by MMI.cpp and iR5900MultDiv.cpp.
				return EmitMMI(op);
			case 0x1e: // LQ, owned by R5900OpcodeImpl.cpp::LQ().
				return EmitLQ(op);
			case 0x1f: // SQ, owned by R5900OpcodeImpl.cpp::SQ().
				return EmitSQ(op);
			case 0x20: // LB, owned by R5900OpcodeImpl.cpp::LB().
				return EmitLB(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x21: // LH, owned by R5900OpcodeImpl.cpp::LH().
				return EmitLH(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x22: // LWL, owned by R5900OpcodeImpl.cpp::LWL().
				return EmitLWL(op);
			case 0x23: // LW, owned by R5900OpcodeImpl.cpp::LW().
				return EmitLW(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x24: // LBU, owned by R5900OpcodeImpl.cpp::LBU().
				return EmitLBU(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x25: // LHU, owned by R5900OpcodeImpl.cpp::LHU().
				return EmitLHU(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x26: // LWR, owned by R5900OpcodeImpl.cpp::LWR().
				return EmitLWR(op);
			case 0x27: // LWU, owned by R5900OpcodeImpl.cpp::LWU().
				return EmitLWU(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x28: // SB, owned by R5900OpcodeImpl.cpp::SB().
				return EmitSB(op);
			case 0x29: // SH, owned by R5900OpcodeImpl.cpp::SH().
				return EmitSH(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x2a: // SWL, owned by R5900OpcodeImpl.cpp::SWL().
				return EmitSWL(op);
			case 0x2b: // SW, owned by R5900OpcodeImpl.cpp::SW().
				return EmitSW(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x2c: // SDL, owned by R5900OpcodeImpl.cpp::SDL().
				return EmitSDL(op);
			case 0x2d: // SDR, owned by R5900OpcodeImpl.cpp::SDR().
				return EmitSDR(op);
			case 0x2e: // SWR, owned by R5900OpcodeImpl.cpp::SWR().
				return EmitSWR(op);
			case 0x2f: // CACHE, owned by Cache.cpp::CACHE().
				return EmitCACHE(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x31: // LWC1, owned by FPU.cpp::LWC1().
				return EmitLWC1(op);
			case 0x33: // PREF, owned by R5900OpcodeImpl.cpp::PREF(); PCSX2 no-ops it.
				return true;
			case 0x36: // LQC2, owned by VU0.cpp::LQC2().
				return EmitLQC2(op);
			case 0x37: // LD, owned by R5900OpcodeImpl.cpp::LD().
				return EmitLD(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x39: // SWC1, owned by FPU.cpp::SWC1().
				return EmitSWC1(op);
			case 0x3e: // SQC2, owned by VU0.cpp::SQC2().
				return EmitSQC2(op);
			case 0x3f: // SD, owned by R5900OpcodeImpl.cpp::SD().
				return EmitSD(op, pc, raw_cycles_through_instruction, event_exit);
			default:
				return false;
		}
	}

	bool BlockCompiler::EndBlockReturn(u8 value)
	{
		return m_code.EmitMovImm8(0, value) &&
			   EmitLinkFrameReturn();
	}

	bool BlockCompiler::EmitLinkFrameReturn()
	{
		return m_code.EmitPop(m_saved_registers | REG_PC);
	}

	bool BlockCompiler::EmitReloadGprPinsAfterClobber(u16 host_mask)
	{
		if (!m_gpr_link_signature.IsValid())
			return true;

		// The exact compatible templates may lend cold-clobbered r1/r3 and the
		// frame-saved LR to iCore mappings. Translation and BL/BLX seams first
		// synchronize dirty mappings where required, then reload only the words
		// whose host registers that seam actually owns.
		for (unsigned i = 0; i < m_pin_count; i++)
		{
			const size_t offset = GprOffset(m_pin_guest[i]);
			if ((host_mask & (1u << m_pin_host[i])) != 0 &&
				!m_code.EmitLdrImm12(m_pin_host[i],
					HOST_CPU_REGS, static_cast<u16>(offset)))
			{
				return false;
			}
			if (m_pin_high_host[i] != NO_GPR_PIN_HOST &&
				(host_mask & (1u << m_pin_high_host[i])) != 0 &&
				!m_code.EmitLdrImm12(m_pin_high_host[i],
					HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32))))
			{
				return false;
			}
		}
		return true;
	}

	bool BlockCompiler::EmitExitToTarget(const void* target, u8 callable_token)
	{
		if (!target)
			return false;
		if (!m_persistent_dispatch_exits)
			return m_code.EmitMovImm8(0, callable_token) && EmitLinkFrameReturn();

		// Persistent exits cannot rely on LR because generated helper calls own it.
		// The dispatcher slab and every EE block share one 8 MiB mapping, so an A32
		// B reaches it directly and avoids an address materialization plus BX.
		const size_t branch = m_code.EmitBranchPlaceholder();
		return branch != static_cast<size_t>(-1) &&
		       m_code.PatchBranchToAddress(branch, target);
	}

	bool BlockCompiler::EmitDirectLinkTail(const void* direct_exit, DirectLinkSlot* direct_link,
		bool defer_pc_writeback, u32 pc, bool sync_private_fallback)
	{
		if (!direct_exit)
			return false;

		const size_t target_offset = m_code.Size();
		const size_t target_branch = m_code.EmitBranchPlaceholder();
		if (target_branch == static_cast<size_t>(-1))
			return false;

		const size_t fallback_offset = m_code.Size();
		if (!m_code.PatchBranch(target_branch, fallback_offset) ||
			(sync_private_fallback && !EmitSyncGprPinsToBacking()) ||
			(defer_pc_writeback && !EmitStorePc(pc)) ||
			!EmitExitToTarget(direct_exit, EE_DIRECT_EXIT_TOKEN))
		{
			return false;
		}

		if (direct_link)
		{
			direct_link->target_offset = target_offset;
			direct_link->fallback_offset = fallback_offset;
			direct_link->branch_on_taken = false;
			direct_link->branch_on_unsigned_less = false;
			direct_link->requires_compatible_entry = sync_private_fallback;
			direct_link->compatible_scheduler_countdown = sync_private_fallback &&
				m_compatible_scheduler_countdown;
			direct_link->compatible_vtlb_pointer = sync_private_fallback &&
				m_compatible_vtlb_pointer;
			direct_link->compatible_words = sync_private_fallback ?
				m_gpr_link_signature.WordCount() : 0;
			direct_link->compatible_dirty_words = sync_private_fallback ?
				m_gpr_link_signature.DirtyWordCount() : 0;
		}
		return true;
	}

	bool BlockCompiler::EmitTakenDirectLinkTail(const void* direct_exit, size_t target_branch,
		DirectLinkSlot* direct_link, bool defer_pc_writeback, u32 pc,
		bool sync_private_fallback)
	{
		if (!direct_exit || target_branch == static_cast<size_t>(-1))
			return false;

		// Reuse the branch-flag selector as the patchable taken link. This
		// preserves the normal fallback return while avoiding a second taken A32
		// branch when the guest branch is taken.
		const size_t fallback_offset = m_code.Size();
		const VitaA32::Condition taken_condition =
			m_deferred_resident_unsigned_branch_suffix ?
				VitaA32::Condition::CC : VitaA32::Condition::NE;
		if (!m_code.PatchBranch(target_branch, fallback_offset, taken_condition) ||
			(sync_private_fallback && !EmitSyncGprPinsToBacking()) ||
			(defer_pc_writeback && !EmitStorePc(pc)) ||
			!EmitExitToTarget(direct_exit, EE_DIRECT_EXIT_TOKEN))
		{
			return false;
		}

		if (direct_link)
		{
			direct_link->target_offset = target_branch;
			direct_link->fallback_offset = fallback_offset;
			direct_link->branch_on_taken = true;
			direct_link->branch_on_unsigned_less =
				m_deferred_resident_unsigned_branch_suffix;
			direct_link->requires_compatible_entry = sync_private_fallback;
			direct_link->compatible_scheduler_countdown = sync_private_fallback &&
				m_compatible_scheduler_countdown;
			direct_link->compatible_vtlb_pointer = sync_private_fallback &&
				m_compatible_vtlb_pointer;
			direct_link->compatible_words = sync_private_fallback ?
				m_gpr_link_signature.WordCount() : 0;
			direct_link->compatible_dirty_words = sync_private_fallback ?
				m_gpr_link_signature.DirtyWordCount() : 0;
		}
		return true;
	}

	bool BlockCompiler::EmitEventExitReturn(const void* event_exit)
	{
		if (!event_exit)
			return false;

		return EmitExitToTarget(event_exit, EE_EVENT_EXIT_TOKEN);
	}

	bool BlockCompiler::EmitDeferredPcWriteback(bool defer_pc_writeback, u32 direct_pc,
		u32 taken_pc, bool conditional_pc, bool indirect_pc_writeback)
	{
		if (!defer_pc_writeback)
			return true;
		if (indirect_pc_writeback)
			return EmitStorePcFromHostReg(HOST_BRANCH_TARGET);

		return conditional_pc ? EmitStoreBranchPc(taken_pc, direct_pc) : EmitStorePc(direct_pc);
	}

	bool BlockCompiler::EmitIndirectDispatchTail(const void* lookup_pages_slot,
		const void* direct_linking_enabled_flag, const void* direct_exit, bool defer_pc_writeback)
	{
		if (!lookup_pages_slot || !direct_linking_enabled_flag || !direct_exit)
			return false;

		if (!m_code.EmitTstImm32(HOST_BRANCH_TARGET, 0x3))
			return false;

		size_t fallback_unaligned = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_unaligned == static_cast<size_t>(-1))
			return false;

		// The slot points at BlockExecutor::m_active_generated_lookup_pages:
		// null while generated linking is disabled, otherwise the populated
		// PCSX2 BaseBlocks-style directory. That makes the disabled case share
		// the same no-directory fallback instead of loading a separate flag.
		if (!m_code.EmitMovImm32(HOST_TMP0, static_cast<u32>(reinterpret_cast<uptr>(lookup_pages_slot))) ||
			!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP0, 0) ||
			!m_code.EmitCmpImm32(HOST_TMP0, 0))
		{
			return false;
		}

		size_t fallback_no_directory = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (fallback_no_directory == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_BRANCH_TARGET, VitaA32::ShiftType::LSR, 16) ||
			!m_code.EmitLdrRegShift(HOST_TMP0, HOST_TMP0, HOST_TMP1, VitaA32::ShiftType::LSL, 2) ||
			!m_code.EmitCmpImm32(HOST_TMP0, 0))
		{
			return false;
		}

		size_t fallback_no_page = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (fallback_no_page == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitUbfx(HOST_TMP1, HOST_BRANCH_TARGET, 2, 14) ||
			!m_code.EmitLdrRegShift(HOST_TMP4, HOST_TMP0, HOST_TMP1, VitaA32::ShiftType::LSL, 2) ||
			!m_code.EmitCmpImm32(HOST_TMP4, 0))
		{
			return false;
		}

		size_t fallback_no_entry = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (fallback_no_entry == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitBx(HOST_TMP4))
		{
			return false;
		}

		const size_t fallback_target = m_code.Size();
		return m_code.PatchBranch(fallback_unaligned, fallback_target, VitaA32::Condition::NE) &&
			   m_code.PatchBranch(fallback_no_directory, fallback_target, VitaA32::Condition::EQ) &&
			   m_code.PatchBranch(fallback_no_page, fallback_target, VitaA32::Condition::EQ) &&
			   m_code.PatchBranch(fallback_no_entry, fallback_target, VitaA32::Condition::EQ) &&
			   (!defer_pc_writeback || EmitStorePcFromHostReg(HOST_BRANCH_TARGET)) &&
			   EmitExitToTarget(direct_exit, EE_DIRECT_EXIT_TOKEN);
	}

	bool BlockCompiler::IsWaitLoopBody(u32 loop_start_pc, u32 loop_end_pc, u32 branch_pc)
	{
		// PCSX2 owner: x86/ix86-32/iR5900.cpp::recRecompile() StartRecomp scan
		// for s_nBlockFF. A self-branching loop whose body never writes a
		// register it already read (except registers refreshed from constants,
		// memory loads, or COP moves each iteration) repeats identically until
		// an event runs, so the taken exit may fast-forward to nextEventCycle.
		// PCSX2 scans [startpc, s_nEndBlock); here the loop head may live in
		// earlier blocks because counter-read loads split A32 blocks, so the
		// scan walks the whole guest loop range and skips only the loop branch.
		constexpr u32 MAX_WAIT_LOOP_BYTES = 128 * 4;
		if (loop_start_pc >= loop_end_pc || (loop_start_pc & 3u) != 0 ||
			loop_end_pc - loop_start_pc > MAX_WAIT_LOOP_BYTES)
		{
			return false;
		}

		u32 reads = 0;
		u32 loads = 1;
		for (u32 i = loop_start_pc; i < loop_end_pc; i += 4)
		{
			if (i == branch_pc)
				continue;

			const u32 code = memRead32(i);
			const u32 opcode = code >> 26;
			const u32 rs = (code >> 21) & 0x1f;
			const u32 rt = (code >> 16) & 0x1f;
			const u32 rd = (code >> 11) & 0x1f;
			const u32 funct = code & 0x3f;

			if (code == 0)
				continue;

			if (opcode == 0x2f || (opcode == 0 && funct == 0x0f))
				continue; // CACHE, SYNC

			if ((opcode & 0x38) == 0x08 || (opcode & 0x3e) == 0x18)
			{
				// ADDI..LUI immediates plus DADDI/DADDIU.
				if (loads & (1u << rs))
	{
					loads |= 1u << rt;
					continue;
	}
				reads |= 1u << rs;
				if (reads & (1u << rt))
					return false;
			}
			else if (opcode == 0 && (funct & 0x30) == 0x20 && (funct & 0x3e) != 0x28)
			{
				// ADD..NOR and SLT..DSUBU register arithmetic.
				if ((loads & (1u << rs)) && (loads & (1u << rt)))
	{
					loads |= 1u << rd;
					continue;
	}
				reads |= (1u << rs) | (1u << rt);
				if (reads & (1u << rd))
					return false;
			}
			else if ((opcode & 0x38) == 0x20 || (opcode & 0x3e) == 0x1a || opcode == 0x37)
			{
				// LB..LWU byte/word loads plus LDL/LDR and LD.
				if (loads & (1u << rs))
	{
					loads |= 1u << rt;
					continue;
	}
				reads |= 1u << rs;
				if (reads & (1u << rt))
					return false;
			}
			else if ((opcode & 0x3c) == 0x10 && rs < 4)
			{
				loads |= 1u << rt; // MFC*/DMFC*/CFC* refresh rt every iteration.
			}
			else
			{
				return false;
			}
		}

		return true;
	}

	bool BlockCompiler::EmitWaitLoopFastForwardTail(const void* event_exit,
		bool defer_pc_writeback, u32 pc)
	{
		// PCSX2 owner: x86/ix86-32/iR5900.cpp::iBranchTest() WaitLoop form:
		// cycle = max(cycle + block cycles, nextEventCycle), then dispatch
		// through the event path. Callers reach this tail with HOST_TMP0
		// holding the freshly stored cycle low word and HOST_TMP2 holding the
		// nextEventCycle low word after an MI (cycle < nextEventCycle)
		// compare, so the max reduces to copying nextEventCycle into cycle.
		// The u64 high word copy is exact under the bounded signed windows of
		// R5900.cpp::cpuSetNextEvent() / cpuTestCycle().
		constexpr u16 cycle_high_offset = static_cast<u16>(CYCLE_OFFSET + sizeof(u32));
		constexpr u16 next_event_high_offset = static_cast<u16>(NEXT_EVENT_OFFSET + sizeof(u32));
		if (!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, next_event_high_offset) ||
			!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, cycle_high_offset))
		{
			return false;
		}

		return (!defer_pc_writeback || EmitStorePc(pc)) && EmitEventExitReturn(event_exit);
	}

	bool BlockCompiler::EndBlockWithWaitLoopFastForward(u32 block_cycles, const void* event_exit)
	{
		if (!event_exit)
			return false;

		if (!EmitFlushDirtyGprPins())
			return false;

#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuWaitLoopFastForwardBlocks++;
#endif

		size_t carry_branch = static_cast<size_t>(-1);
		if (!EmitAddScaledCyclesToCpuLowWord(block_cycles, HOST_TMP0, HOST_TMP2, &carry_branch))
			return false;

		const size_t cycle_compare_target = m_code.Size();
		if (!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(NEXT_EVENT_OFFSET)) ||
			!m_code.EmitSubReg(HOST_TMP1, HOST_TMP0, HOST_TMP2, true))
		{
			return false;
		}

		const size_t no_skip = m_code.EmitBranchPlaceholder(VitaA32::Condition::PL);
		if (no_skip == static_cast<size_t>(-1))
			return false;

		if (!EmitWaitLoopFastForwardTail(event_exit))
			return false;

		if (!m_code.PatchBranch(no_skip, m_code.Size(), VitaA32::Condition::PL) ||
			!EmitEventExitReturn(event_exit))
		{
			return false;
		}

		const size_t carry_branches[] = {carry_branch};
		return EmitCycleCarryFixup(carry_branches, 1, cycle_compare_target, HOST_TMP1);
	}

	bool BlockCompiler::EndBlockWithCycleTest(u32 block_cycles, const void* direct_exit, const void* event_exit,
		DirectLinkSlot* direct_link, DirectLinkSlot* taken_link,
		const void* indirect_lookup_pages_slot, const void* direct_linking_enabled_flag,
		bool wait_loop_taken, bool defer_pc_writeback,
		u32 direct_pc, u32 taken_pc, bool conditional_pc, bool indirect_pc_writeback,
		bool preserve_dirty_direct_link, bool preserve_dirty_taken_link)
	{
		if (!direct_exit || !event_exit)
			return false;
		// Wait-loop fast-forward has a different cycle/publication contract.
		// Both resident analyses reject it during construction; fail closed if
		// those analyses ever disagree with the final whole-loop scan.
		if ((m_resident_scheduler_countdown || m_compatible_scheduler_countdown) &&
			wait_loop_taken)
			return false;

		// The forwarded boolean is part of the same resident self-link contract
		// even when a future matching block has no dirty scalar pin of its own.
		const bool carry_dirty_direct_link = preserve_dirty_direct_link && direct_link;
		const bool carry_dirty_taken_link =
			(preserve_dirty_taken_link || m_forwarded_boolean_branch) &&
			taken_link && !wait_loop_taken;
		const bool carry_dirty_link = carry_dirty_direct_link || carry_dirty_taken_link;
		if (!carry_dirty_link && !EmitFlushDirtyGprPins())
			return false;

#if defined(VITASX2_QEMU_VALIDATION)
		if (wait_loop_taken)
			g_qemuWaitLoopFastForwardBlocks++;
#endif

		size_t carry_branch = static_cast<size_t>(-1);
		if (m_compatible_scheduler_countdown)
		{
			if (!m_code.EmitAddImm32(GprLinkSignature::SCHEDULER_HOST,
					GprLinkSignature::SCHEDULER_HOST, block_cycles, true) &&
				(!m_code.EmitMovImm32(HOST_TMP1, block_cycles) ||
				 !m_code.EmitAddReg(GprLinkSignature::SCHEDULER_HOST,
					 GprLinkSignature::SCHEDULER_HOST, HOST_TMP1, true)))
			{
				return false;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuResidentCycleLowHotInstructionsElided += 3;
			g_qemuResidentNextEventLowHotInstructionsElided++;
			g_qemuResidentSchedulerCountdownHotInstructionsElided++;
#endif
		}
		else if (m_resident_scheduler_countdown)
		{
			const size_t countdown_start = m_code.Size();
			if (!m_code.EmitAddImm32(HOST_TMP2, HOST_TMP2, block_cycles, true) &&
				(!m_code.EmitMovImm32(HOST_TMP1, block_cycles) ||
				 !m_code.EmitAddReg(HOST_TMP2, HOST_TMP2, HOST_TMP1, true)))
			{
				return false;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			const u32 countdown_instructions = static_cast<u32>(
				(m_code.Size() - countdown_start) / sizeof(u32));
			g_qemuResidentCycleLowHotInstructionsElided +=
				4 > countdown_instructions ? 4 - countdown_instructions : 0;
			g_qemuResidentNextEventLowHotInstructionsElided++;
			g_qemuResidentSchedulerCountdownHotInstructionsElided++;
#endif
		}
		else if (m_resident_cycle_low)
		{
			const size_t add_start = m_code.Size();
			if (!m_code.EmitAddImm32(HOST_TMP0, HOST_TMP0, block_cycles) &&
				(!m_code.EmitMovImm32(HOST_TMP2, block_cycles) ||
				 !m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2)))
			{
				return false;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			const u32 resident_add_instructions = static_cast<u32>(
				(m_code.Size() - add_start) / sizeof(u32));
			g_qemuResidentCycleLowHotInstructionsElided +=
				4 > resident_add_instructions ? 4 - resident_add_instructions : 0;
#endif
		}
		else if (!EmitAddScaledCyclesToCpuLowWord(
			block_cycles, HOST_TMP0, HOST_TMP2, &carry_branch))
		{
			return false;
		}

		// Mirrors PCSX2's normal x86/ix86-32/iR5900.cpp::iBranchTest() path.
		// Scheduler deltas are bounded to signed 32-bit windows
		// (`R5900.cpp::cpuSetNextEvent()` / `cpuTestCycle()`), so the hot path
		// compares the low-word delta and only fixes the u64 high word on wrap.
		// Wait-loop blocks keep the nextEventCycle low word live in HOST_TMP2
		// for the fast-forward taken tail.
		const size_t cycle_compare_target = m_code.Size();
		if (!m_resident_scheduler_countdown && !m_compatible_scheduler_countdown)
		{
			if (m_resident_cycle_low && !wait_loop_taken)
			{
				if (!m_code.EmitSubReg(HOST_TMP1, HOST_TMP0, HOST_TMP2, true))
					return false;
			}
			else if (!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS,
					 static_cast<u16>(NEXT_EVENT_OFFSET)) ||
				 !m_code.EmitSubReg(wait_loop_taken ? HOST_TMP1 : HOST_TMP2,
					 HOST_TMP0, HOST_TMP2, true))
			{
				return false;
			}
		}
		// Otherwise the flag-setting countdown ADD above already produced exactly
		// the signed cycle.low-nextEventCycle.low predicate consumed by BPL.

		// Scheduler events are rare relative to block dispatch. Keep PCSX2's
		// signed-delta test, but invert the A32 layout so cycle < nextEventCycle
		// falls through into the direct tail instead of taking a hot branch.
		const size_t event_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::PL);
		if (event_branch == static_cast<size_t>(-1))
			return false;
		if (taken_link || wait_loop_taken)
		{
			if (m_deferred_resident_unsigned_branch_suffix)
			{
				if (!EmitDeferredResidentUnsignedBranchSuffix())
					return false;
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuResidentUnsignedBranchSuffixHotInstructionsElided++;
#endif
			}
			else if (!m_code.EmitCmpImm32(m_branch_flag_host, 0))
			{
				return false;
			}

			const VitaA32::Condition taken_condition =
				m_deferred_resident_unsigned_branch_suffix ?
					VitaA32::Condition::CC : VitaA32::Condition::NE;
			const size_t taken_tail = m_code.EmitBranchPlaceholder(taken_condition);
			if (taken_tail == static_cast<size_t>(-1))
				return false;

			// The fall-through edge leaves this allocator contract. When the taken
			// self-edge carries dirty pins, keep backing state authoritative before
			// even a patched fall-through link.
			if ((!carry_dirty_direct_link && carry_dirty_link &&
					!EmitSyncGprPinsToBacking()) ||
				!EmitDirectLinkTail(direct_exit, direct_link, defer_pc_writeback, direct_pc,
					carry_dirty_direct_link))
				return false;

			// PCSX2 owner: iBranchTest()'s WaitLoop form applies only to the
			// tail whose newpc is the loop head (s_branchTo), i.e. the taken
			// side of the loop branch.
			bool taken_tail_ok = false;
			if (wait_loop_taken)
			{
				const size_t taken_tail_target = m_code.Size();
				taken_tail_ok = m_code.PatchBranch(taken_tail, taken_tail_target, VitaA32::Condition::NE) &&
					EmitWaitLoopFastForwardTail(event_exit, defer_pc_writeback, taken_pc);
			}
			else
			{
				taken_tail_ok = EmitTakenDirectLinkTail(direct_exit, taken_tail, taken_link,
					defer_pc_writeback, taken_pc, carry_dirty_link);
			}

			const size_t event_target = m_code.Size();
			const size_t carry_branches[] = {carry_branch};
			if (!taken_tail_ok ||
				!m_code.PatchBranch(event_branch, event_target, VitaA32::Condition::PL) ||
				(m_deferred_resident_unsigned_branch_suffix &&
					!EmitDeferredResidentUnsignedBranchSuffix(true)) ||
				(carry_dirty_link && !EmitSyncGprPinsToBacking()) ||
				!EmitDeferredPcWriteback(defer_pc_writeback, direct_pc, taken_pc, conditional_pc,
					indirect_pc_writeback) ||
				!EmitEventExitReturn(event_exit) ||
				!EmitCycleCarryFixup(carry_branches, 1, cycle_compare_target, HOST_TMP1))
			{
				return false;
			}

#if defined(VITASX2_QEMU_VALIDATION)
			if (m_deferred_resident_unsigned_branch_suffix)
				g_qemuResidentUnsignedBranchSuffixEventInstructions += 5;
#endif

			return true;
		}

		if (direct_link)
		{
			if (!EmitDirectLinkTail(direct_exit, direct_link, defer_pc_writeback, direct_pc,
					carry_dirty_link))
			{
				return false;
			}
		}
		else if (indirect_lookup_pages_slot && direct_linking_enabled_flag)
		{
			if (!EmitIndirectDispatchTail(indirect_lookup_pages_slot, direct_linking_enabled_flag,
					direct_exit, defer_pc_writeback))
			{
				return false;
			}
		}
		else if (!EmitDeferredPcWriteback(defer_pc_writeback, direct_pc, taken_pc, conditional_pc,
			indirect_pc_writeback) ||
			!EmitExitToTarget(direct_exit, EE_DIRECT_EXIT_TOKEN))
		{
			return false;
		}

		const size_t event_target = m_code.Size();
		const size_t carry_branches[] = {carry_branch};
		return m_code.PatchBranch(event_branch, event_target, VitaA32::Condition::PL) &&
			   (!carry_dirty_link || EmitSyncGprPinsToBacking()) &&
			   EmitDeferredPcWriteback(defer_pc_writeback, direct_pc, taken_pc, conditional_pc,
				   indirect_pc_writeback) &&
			   EmitEventExitReturn(event_exit) &&
			   EmitCycleCarryFixup(carry_branches, 1, cycle_compare_target, HOST_TMP1);
	}

	bool BlockCompiler::EndBlockWithLikelyCycleTest(u32 taken_cycles, u32 not_taken_cycles,
		const void* direct_exit, const void* event_exit, DirectLinkSlot* not_taken_link,
		DirectLinkSlot* taken_link, bool wait_loop_taken,
		bool defer_pc_writeback, u32 not_taken_pc, u32 taken_pc,
		bool preserve_dirty_not_taken_link, bool preserve_dirty_taken_link)
	{
		if (!direct_exit || !event_exit)
			return false;

		const bool carry_dirty_not_taken_link =
			preserve_dirty_not_taken_link && not_taken_link;
		const bool carry_dirty_taken_link =
			preserve_dirty_taken_link && taken_link && !wait_loop_taken;
		const bool carry_dirty_link =
			carry_dirty_not_taken_link || carry_dirty_taken_link;
		if (!carry_dirty_link && !EmitFlushDirtyGprPins())
			return false;

#if defined(VITASX2_QEMU_VALIDATION)
		if (wait_loop_taken)
			g_qemuWaitLoopFastForwardBlocks++;
#endif

		const auto add_cycles = [this](u32 cycles, size_t* carry_branch) {
			if (!m_code.EmitAddImm32(HOST_TMP0, HOST_TMP0, cycles, true))
			{
				if (!m_code.EmitMovImm32(HOST_TMP2, cycles) ||
					!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true))
	{
					return false;
	}
			}

			if (!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)))
				return false;

			*carry_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
			return *carry_branch != static_cast<size_t>(-1);
		};

		size_t not_taken_carry_branch = static_cast<size_t>(-1);
		size_t taken_carry_branch = static_cast<size_t>(-1);
		const u32 cycle_delta = (taken_cycles >= not_taken_cycles) ?
			(taken_cycles - not_taken_cycles) : UINT32_MAX;
		const bool delta_is_power_of_two = cycle_delta != 0 &&
			(cycle_delta & (cycle_delta - 1)) == 0;
		const bool delta_is_one_plus_power_of_two = cycle_delta > 1 &&
			((cycle_delta - 1) & (cycle_delta - 2)) == 0;
		if (m_compatible_scheduler_countdown)
		{
			if (taken_cycles == not_taken_cycles)
			{
				if (!m_code.EmitAddImm32(GprLinkSignature::SCHEDULER_HOST,
						GprLinkSignature::SCHEDULER_HOST, taken_cycles, true) &&
					(!m_code.EmitMovImm32(HOST_TMP2, taken_cycles) ||
					 !m_code.EmitAddReg(GprLinkSignature::SCHEDULER_HOST,
						 GprLinkSignature::SCHEDULER_HOST, HOST_TMP2, true)))
				{
					return false;
				}
			}
			else if (taken_cycles >= not_taken_cycles &&
				(cycle_delta == 1 || delta_is_power_of_two ||
				 delta_is_one_plus_power_of_two))
			{
				if (cycle_delta == 1)
				{
					if (!m_code.EmitAddImm32(HOST_TMP2, m_branch_flag_host,
							not_taken_cycles) &&
						(!m_code.EmitMovImm32(HOST_TMP2, not_taken_cycles) ||
						 !m_code.EmitAddReg(HOST_TMP2, m_branch_flag_host, HOST_TMP2)))
					{
						return false;
					}
				}
				else
				{
					const u32 shifted_delta =
						delta_is_power_of_two ? cycle_delta : cycle_delta - 1;
					u8 shift = 0;
					while ((1u << shift) != shifted_delta)
						shift++;
					const bool formed_delta = delta_is_power_of_two ?
						m_code.EmitMovRegShiftImm(HOST_TMP2, m_branch_flag_host,
							VitaA32::ShiftType::LSL, shift) :
						m_code.EmitAddRegShiftImm(HOST_TMP2, m_branch_flag_host,
							m_branch_flag_host, VitaA32::ShiftType::LSL, shift);
					if (!formed_delta ||
						(!m_code.EmitAddImm32(HOST_TMP2, HOST_TMP2, not_taken_cycles) &&
						 (!m_code.EmitMovImm32(HOST_TMP1, not_taken_cycles) ||
						  !m_code.EmitAddReg(HOST_TMP2, HOST_TMP2, HOST_TMP1))))
					{
						return false;
					}
				}
				if (!m_code.EmitAddReg(GprLinkSignature::SCHEDULER_HOST,
						GprLinkSignature::SCHEDULER_HOST, HOST_TMP2, true))
				{
					return false;
				}
			}
			else if (!m_code.EmitMovImm32(HOST_TMP2, not_taken_cycles) ||
				!m_code.EmitCmpImm32(m_branch_flag_host, 0) ||
				!m_code.EmitMovImm32(HOST_TMP2, taken_cycles, VitaA32::Condition::NE) ||
				!m_code.EmitAddReg(GprLinkSignature::SCHEDULER_HOST,
					GprLinkSignature::SCHEDULER_HOST, HOST_TMP2, true))
			{
				return false;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuResidentCycleLowHotInstructionsElided += 3;
			g_qemuResidentNextEventLowHotInstructionsElided++;
			g_qemuResidentSchedulerCountdownHotInstructionsElided++;
#endif
		}
		else if (taken_cycles == not_taken_cycles)
		{
			if (!EmitAddScaledCyclesToCpuLowWord(taken_cycles, HOST_TMP0, HOST_TMP2,
					&not_taken_carry_branch))
			{
				return false;
			}
		}
		else if (cycle_delta == 1 || delta_is_power_of_two || delta_is_one_plus_power_of_two)
		{
			// Every dynamic branch emitter normalizes the branch flag to zero or
			// one. Fold the scaled delay-slot delta into the addend when A32 can
			// form flag * delta with one barrel-shifted instruction, rather than
			// branching between duplicate add/store paths. The common default-op
			// delta is three: flag + (flag << 1).
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)))
				return false;

			if (cycle_delta == 1)
			{
				if (!m_code.EmitAddImm32(HOST_TMP2, m_branch_flag_host, not_taken_cycles) &&
					(!m_code.EmitMovImm32(HOST_TMP2, not_taken_cycles) ||
					 !m_code.EmitAddReg(HOST_TMP2, m_branch_flag_host, HOST_TMP2)))
	{
					return false;
	}
			}
			else
			{
				const u32 shifted_delta = delta_is_power_of_two ? cycle_delta : cycle_delta - 1;
				u8 shift = 0;
				while ((1u << shift) != shifted_delta)
					shift++;

				const bool formed_delta = delta_is_power_of_two ?
					m_code.EmitMovRegShiftImm(HOST_TMP2, m_branch_flag_host, VitaA32::ShiftType::LSL, shift) :
					m_code.EmitAddRegShiftImm(HOST_TMP2, m_branch_flag_host, m_branch_flag_host,
						VitaA32::ShiftType::LSL, shift);
				if (!formed_delta ||
					(!m_code.EmitAddImm32(HOST_TMP2, HOST_TMP2, not_taken_cycles) &&
					 (!m_code.EmitMovImm32(HOST_TMP1, not_taken_cycles) ||
					  !m_code.EmitAddReg(HOST_TMP2, HOST_TMP2, HOST_TMP1))))
	{
					return false;
	}
			}

			if (!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)))
			{
				return false;
			}

			not_taken_carry_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
			if (not_taken_carry_branch == static_cast<size_t>(-1))
				return false;
		}
		else
		{
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)) ||
				!m_code.EmitCmpImm32(m_branch_flag_host, 0))
			{
				return false;
			}

			const size_t taken_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (taken_path == static_cast<size_t>(-1) ||
				!add_cycles(not_taken_cycles, &not_taken_carry_branch))
			{
				return false;
			}

			const size_t cycles_done = m_code.EmitBranchPlaceholder();
			if (cycles_done == static_cast<size_t>(-1))
				return false;

			const size_t taken_target = m_code.Size();
			if (!m_code.PatchBranch(taken_path, taken_target, VitaA32::Condition::NE) ||
				!add_cycles(taken_cycles, &taken_carry_branch) ||
				!m_code.PatchBranch(cycles_done, m_code.Size()))
			{
				return false;
			}
		}

		const size_t cycle_compare_target = m_code.Size();
		if (!m_compatible_scheduler_countdown &&
			(!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS,
				 static_cast<u16>(NEXT_EVENT_OFFSET)) ||
			 !m_code.EmitSubReg(wait_loop_taken ? HOST_TMP1 : HOST_TMP2,
				 HOST_TMP0, HOST_TMP2, true)))
		{
			return false;
		}

		// Match the normal cycle-test layout: direct dispatch is the common
		// fallthrough, while cycle >= nextEventCycle takes the event branch.
		const size_t event_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::PL);
		if (event_branch == static_cast<size_t>(-1))
			return false;
		if (not_taken_link || taken_link || wait_loop_taken)
		{
			if (!m_code.EmitCmpImm32(m_branch_flag_host, 0))
				return false;

			const size_t taken_tail = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (taken_tail == static_cast<size_t>(-1))
				return false;

			if ((!carry_dirty_not_taken_link && carry_dirty_link &&
					!EmitSyncGprPinsToBacking()) ||
				!EmitDirectLinkTail(direct_exit, not_taken_link,
					defer_pc_writeback, not_taken_pc, carry_dirty_not_taken_link))
				return false;

			// PCSX2 owner: iBranchTest()'s WaitLoop form applies only to the
			// taken (loop head, s_branchTo) tail of likely loop branches.
			bool taken_tail_ok = false;
			if (wait_loop_taken)
			{
				const size_t taken_tail_target = m_code.Size();
				taken_tail_ok = m_code.PatchBranch(taken_tail, taken_tail_target, VitaA32::Condition::NE) &&
					EmitWaitLoopFastForwardTail(event_exit, defer_pc_writeback, taken_pc);
			}
			else
			{
				taken_tail_ok = EmitTakenDirectLinkTail(direct_exit, taken_tail, taken_link,
					defer_pc_writeback, taken_pc, carry_dirty_link);
			}

			const size_t event_target = m_code.Size();
			const size_t carry_branches[] = {not_taken_carry_branch, taken_carry_branch};
			if (!taken_tail_ok ||
				!m_code.PatchBranch(event_branch, event_target, VitaA32::Condition::PL) ||
				(carry_dirty_link && !EmitSyncGprPinsToBacking()) ||
				!EmitDeferredPcWriteback(defer_pc_writeback, not_taken_pc, taken_pc, true) ||
				!EmitEventExitReturn(event_exit) ||
				!EmitCycleCarryFixup(carry_branches, 2, cycle_compare_target, HOST_TMP1))
			{
				return false;
			}

			return true;
		}

		const size_t carry_branches[] = {not_taken_carry_branch, taken_carry_branch};
		if ((carry_dirty_link && !EmitSyncGprPinsToBacking()) ||
			!EmitDeferredPcWriteback(defer_pc_writeback, not_taken_pc, taken_pc, true) ||
			!EmitExitToTarget(direct_exit, EE_DIRECT_EXIT_TOKEN))
		{
			return false;
		}

		const size_t event_target = m_code.Size();
		return m_code.PatchBranch(event_branch, event_target, VitaA32::Condition::PL) &&
			   (!carry_dirty_link || EmitSyncGprPinsToBacking()) &&
			   EmitDeferredPcWriteback(defer_pc_writeback, not_taken_pc, taken_pc, true) &&
			   EmitEventExitReturn(event_exit) &&
			   EmitCycleCarryFixup(carry_branches, 2, cycle_compare_target, HOST_TMP1);
	}

	bool BlockCompiler::EmitSPECIAL(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		switch (op & 0x3f)
		{
			case 0x00: // SLL, owned by R5900OpcodeImpl.cpp::SLL().
				return EmitSLL(op);
			case 0x02: // SRL, owned by R5900OpcodeImpl.cpp::SRL().
				return EmitSRL(op);
			case 0x03: // SRA, owned by R5900OpcodeImpl.cpp::SRA().
				return EmitSRA(op);
			case 0x04: // SLLV, owned by R5900OpcodeImpl.cpp::SLLV().
				return EmitSLLV(op);
			case 0x06: // SRLV, owned by R5900OpcodeImpl.cpp::SRLV().
				return EmitSRLV(op);
			case 0x07: // SRAV, owned by R5900OpcodeImpl.cpp::SRAV().
				return EmitSRAV(op);
			case 0x0a: // MOVZ, owned by R5900OpcodeImpl.cpp::MOVZ().
				return EmitMOVZ(op);
			case 0x0b: // MOVN, owned by R5900OpcodeImpl.cpp::MOVN().
				return EmitMOVN(op);
			case 0x0c: // SYSCALL, owned by R5900OpcodeImpl.cpp::SYSCALL().
				return EmitSYSCALL(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x0d: // BREAK, owned by R5900OpcodeImpl.cpp::BREAK().
				return EmitBREAK(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x0f: // SYNC, owned by R5900OpcodeImpl.cpp::SYNC(); PCSX2 no-ops it.
				return true;
			case 0x10: // MFHI, owned by R5900OpcodeImpl.cpp::MFHI().
				return EmitMFHI(op);
			case 0x11: // MTHI, owned by R5900OpcodeImpl.cpp::MTHI().
				return EmitMTHI(op);
			case 0x12: // MFLO, owned by R5900OpcodeImpl.cpp::MFLO().
				return EmitMFLO(op);
			case 0x13: // MTLO, owned by R5900OpcodeImpl.cpp::MTLO().
				return EmitMTLO(op);
			case 0x14: // DSLLV, owned by R5900OpcodeImpl.cpp::DSLLV().
				return EmitDSLLV(op);
			case 0x16: // DSRLV, owned by R5900OpcodeImpl.cpp::DSRLV().
				return EmitDSRLV(op);
			case 0x17: // DSRAV, owned by R5900OpcodeImpl.cpp::DSRAV().
				return EmitDSRAV(op);
			case 0x18: // MULT, owned by R5900OpcodeImpl.cpp::MULT().
				return EmitMULT(op);
			case 0x19: // MULTU, owned by R5900OpcodeImpl.cpp::MULTU().
				return EmitMULTU(op);
			case 0x1a: // DIV, owned by R5900OpcodeImpl.cpp::DIV().
				return EmitDIV(op);
			case 0x1b: // DIVU, owned by R5900OpcodeImpl.cpp::DIVU().
				return EmitDIVU(op);
			case 0x20: // ADD, owned by R5900OpcodeImpl.cpp::ADD(). The PCSX2
				// recompiler owner x86/ix86-32/iR5900Arit.cpp::recADD_() drops the
				// integer-overflow exception, so ADD compiles exactly like ADDU.
				return EmitADDU(op);
			case 0x21: // ADDU, owned by R5900OpcodeImpl.cpp::ADDU().
				return EmitADDU(op);
			case 0x22: // SUB, owned by R5900OpcodeImpl.cpp::SUB(); overflow trap
				// dropped by x86/ix86-32/iR5900Arit.cpp::recSUB_(), compiled as SUBU.
				return EmitSUBU(op);
			case 0x23: // SUBU, owned by R5900OpcodeImpl.cpp::SUBU().
				return EmitSUBU(op);
			case 0x24: // AND, owned by R5900OpcodeImpl.cpp::AND().
				return EmitAND(op);
			case 0x25: // OR, owned by R5900OpcodeImpl.cpp::OR().
				return EmitOR(op);
			case 0x26: // XOR, owned by R5900OpcodeImpl.cpp::XOR().
				return EmitXOR(op);
			case 0x27: // NOR, owned by R5900OpcodeImpl.cpp::NOR().
				return EmitNOR(op);
			case 0x28: // MFSA, owned by R5900OpcodeImpl.cpp::MFSA().
				return EmitMFSA(op);
			case 0x29: // MTSA, owned by R5900OpcodeImpl.cpp::MTSA().
				return EmitMTSA(op);
			case 0x2a: // SLT, owned by R5900OpcodeImpl.cpp::SLT().
				return EmitSLT(op);
			case 0x2b: // SLTU, owned by R5900OpcodeImpl.cpp::SLTU().
				return EmitSLTU(op);
			case 0x2c: // DADD, owned by R5900OpcodeImpl.cpp::DADD(); overflow trap
				// dropped by x86/ix86-32/iR5900Arit.cpp::recDADD_(), compiled as DADDU.
				return EmitDADDU(op);
			case 0x2d: // DADDU, owned by R5900OpcodeImpl.cpp::DADDU().
				return EmitDADDU(op);
			case 0x2e: // DSUB, owned by R5900OpcodeImpl.cpp::DSUB(); overflow trap
				// dropped by x86/ix86-32/iR5900Arit.cpp::recDSUB_(), compiled as DSUBU.
				return EmitDSUBU(op);
			case 0x2f: // DSUBU, owned by R5900OpcodeImpl.cpp::DSUBU().
				return EmitDSUBU(op);
			case 0x30: // TGE, owned by R5900OpcodeImpl.cpp::TGE().
			case 0x31: // TGEU, owned by R5900OpcodeImpl.cpp::TGEU().
			case 0x32: // TLT, owned by R5900OpcodeImpl.cpp::TLT().
			case 0x33: // TLTU, owned by R5900OpcodeImpl.cpp::TLTU().
			case 0x34: // TEQ, owned by R5900OpcodeImpl.cpp::TEQ().
			case 0x36: // TNE, owned by R5900OpcodeImpl.cpp::TNE().
				return EmitTrapEventExit(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x38: // DSLL, owned by R5900OpcodeImpl.cpp::DSLL().
				return EmitDSLL(op);
			case 0x3a: // DSRL, owned by R5900OpcodeImpl.cpp::DSRL().
				return EmitDSRL(op);
			case 0x3b: // DSRA, owned by R5900OpcodeImpl.cpp::DSRA().
				return EmitDSRA(op);
			case 0x3c: // DSLL32, owned by R5900OpcodeImpl.cpp::DSLL32().
				return EmitDSLL32(op);
			case 0x3e: // DSRL32, owned by R5900OpcodeImpl.cpp::DSRL32().
				return EmitDSRL32(op);
			case 0x3f: // DSRA32, owned by R5900OpcodeImpl.cpp::DSRA32().
				return EmitDSRA32(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitCOP0(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		// PCSX2 owner: x86/iCOP0.cpp. CP0_RECOMPILE keeps ordinary MFC0/MTC0,
		// Count, perf-counter direct cases, straight-line delayed DI, and TLB
		// read/probe ops inside generated code. MTC0 Status calls
		// WriteCP0Status() in-block after committing cycles, and EI emits its
		// Status.EIE/event scheduling directly before the required event tail.
		// TLB writes, ERET, and the remaining perf helpers keep the event tail
		// until their exact side effects are ported directly.
		using namespace R5900::Interpreter::OpcodeImpl::COP0;
			switch ((op >> 21) & 0x1f)
			{
				case 0x00: // MFC0, owned by COP0.cpp::MFC0().
					if (IsFastMFC0(op))
						return EmitMFC0Fast(op, raw_cycles_through_instruction);
					return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction,
						reinterpret_cast<const void*>(&MFC0), event_exit);
				case 0x04: // MTC0, owned by COP0.cpp::MTC0().
					if (IsFastMTC0(op))
						return EmitMTC0Fast(op, raw_cycles_through_instruction);
					return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction,
						reinterpret_cast<const void*>(&MTC0), event_exit);
			case 0x10: // COP0_C0 class, owned by R5900OpcodeTables.cpp::tbl_COP0_C0.
				switch (op & 0x3f)
	{
					case 0x01: // TLBR, owned by COP0.cpp::TLBR().
						return EmitTLBRInBlock();
					case 0x02: // TLBWI, owned by COP0.cpp::TLBWI().
						return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction,
							reinterpret_cast<const void*>(&TLBWI), event_exit, true);
					case 0x06: // TLBWR, owned by COP0.cpp::TLBWR().
						return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction,
							reinterpret_cast<const void*>(&TLBWR), event_exit, true);
					case 0x08: // TLBP, owned by COP0.cpp::TLBP().
						return EmitTLBPInBlock();
					case 0x18: // ERET, owned by COP0.cpp::ERET().
						return EmitERETEventExit(op, raw_cycles_through_instruction, event_exit);
					case 0x38: // EI, owned by COP0.cpp::EI().
						return EmitEIEventExit(op, pc + 4, raw_cycles_through_instruction, event_exit);
					default:
						return false;
	}
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitMFC0Fast(u32 op, u32 raw_cycles_through_instruction)
	{
		// PCSX2 owner: x86/iCOP0.cpp::recMFC0() under CP0_RECOMPILE. For all
		// ordinary CP0 registers, Count, and MFPS/PCCR, it stays inside the block.
		// PCR0/PCR1 reads still use the helper/event tail to run COP0_UpdatePCCR().
		// rd 24 only logs in PCSX2, so it is a no-op here.
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		if (rd == 9)
			return EmitMFC0CountFast(op, ScaleBlockCycles(raw_cycles_through_instruction));

		if (rt == 0 || rd == 24)
			return true;

		if (rd == 25)
		{
			if ((op & 1u) != 0)
				return EmitMFC0PerfCounterFast(op, ScaleBlockCycles(raw_cycles_through_instruction));

			const unsigned result_reg = SelectGprLowResultHost(rt, HOST_TMP0);
			return m_code.EmitLdrImm12(result_reg, HOST_CPU_REGS, static_cast<u16>(PERF_OFFSET)) &&
				   EmitStoreGprSignExtended32FromLow(rt, result_reg);
		}

		const size_t cp0_offset = Cp0Offset(rd);
		const unsigned result_reg = SelectGprLowResultHost(rt, HOST_TMP0);
		return m_code.EmitLdrImm12(result_reg, HOST_CPU_REGS, static_cast<u16>(cp0_offset)) &&
			   EmitStoreGprSignExtended32FromLow(rt, result_reg);
	}

		bool BlockCompiler::EmitMFC0CountFast(u32 op, u32 scaled_cycles_through_instruction)
		{
		// PCSX2 owner: x86/iCOP0.cpp::recMFC0() rd 9. It commits cycles through
		// scaleblockcycles_clear(), updates CP0.Count from cycle-lastCOP0Cycle
		// even when RT is zero, then returns the sign-extended Count value.
		const unsigned rt = RT(op);
		if (scaled_cycles_through_instruction == 0)
			return false;

		if (!EmitLoadCpuRegsU64(CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP2))
		{
			return false;
		}

		if (!m_code.EmitAddImm32(HOST_TMP0, HOST_TMP0, scaled_cycles_through_instruction, true))
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, scaled_cycles_through_instruction) ||
				!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true))
			{
				return false;
			}
		}

		if (!m_code.EmitAdcImm8(HOST_TMP1, HOST_TMP1, 0) ||
			!EmitStoreCpuRegsU64(CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP2) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(LAST_COP0_CYCLE_OFFSET)) ||
			!m_code.EmitSubReg(HOST_TMP2, HOST_TMP0, HOST_TMP2) ||
			!m_code.EmitLdrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(9))) ||
			!m_code.EmitAddReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
			!m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(9))) ||
			!EmitStoreCpuRegsU64(LAST_COP0_CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP4))
		{
			return false;
		}

		if (rt == 0)
			return true;

			return EmitStoreGprSignExtended32FromLow(rt, HOST_TMP3);
		}

		bool BlockCompiler::EmitMFC0PerfCounterFast(u32 op, u32 scaled_cycles_through_instruction)
		{
			// PCSX2 owner: x86/iCOP0.cpp::recMFC0() rd 25 odd selectors. MFPC0
			// and MFPC1 commit cycles, call COP0_UpdatePCCR(), then sign-extend
			// the selected PCR into the target GPR.
			const unsigned rt = RT(op);
			if (rt == 0)
				return true;
			if (scaled_cycles_through_instruction == 0)
				return false;

			const bool pcr1 = (op & 2u) != 0;
			const size_t pcr_offset = pcr1 ? PERF_PCR1_OFFSET : PERF_PCR0_OFFSET;
			const unsigned result_reg = SelectGprLowResultHost(rt, HOST_TMP0);
			return EmitAddScaledCyclesToCpu(scaled_cycles_through_instruction) &&
				   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&COP0_UpdatePCCR)) &&
				   m_code.EmitLdrImm12(result_reg, HOST_CPU_REGS, static_cast<u16>(pcr_offset)) &&
				   EmitStoreGprSignExtended32FromLow(rt, result_reg);
		}

		bool BlockCompiler::EmitMTC0Fast(u32 op, u32 raw_cycles_through_instruction)
		{
			// PCSX2 owner: x86/iCOP0.cpp::recMTC0() under CP0_RECOMPILE.
			// Status calls PCSX2's WriteCP0Status() in-block after committing
			// cycles, matching the x86 helper-call shape while avoiding an
			// artificial event-tail split. Count and the perf counter writes
			// commit cycles here using the same scaleblockcycles_clear() cadence;
			// MTPS/PCCR calls COP0_UpdatePCCR() before storing the new PCCR and
			// then runs COP0_DiagnosticPCCR(), as PCSX2's x86 path does.
			const unsigned rt = RT(op);
			const unsigned rd = RD(op);
			const auto load_rt_low = [this, rt](unsigned host_reg) {
				u32 value = 0;
				const bool value_known = TryGetKnownGprLow(rt, &value);
				if (rt != 0 && FindGprPinHost(rt) < 0 && value_known)
	{
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuCop0KnownSourceFastPaths++;
#endif
					return m_code.EmitMovImm32(host_reg, value);
	}

				return EmitLoadGprLow(rt, host_reg);
			};

			switch (rd)
			{
				case 0x09: // Count
	{
					const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
					if (cycles == 0 ||
						!EmitAddScaledCyclesToCpu(cycles) ||
						!load_rt_low(HOST_TMP2))
	{
						return false;
	}

					return m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(9))) &&
						   EmitStoreCpuRegsU64(LAST_COP0_CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP4);
	}
				case 0x0c: // Status
	{
					const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
					if (cycles == 0 ||
						!EmitAddScaledCyclesToCpu(cycles) ||
						!load_rt_low(HOST_TMP0))
	{
						return false;
	}

					return m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&WriteCP0Status));
	}
				case 0x10: // Config
					return load_rt_low(HOST_TMP0) &&
						   EmitBicImm32OrReg(HOST_TMP0, HOST_TMP0, 0x00000fc0u, HOST_TMP1) &&
						   EmitOrrImm32OrReg(HOST_TMP0, HOST_TMP0, 0x00000440u, HOST_TMP1) &&
						   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(16)));
				case 0x18: // Breakpoint debug registers
					return true;
				case 0x19: // Perf counters
					if ((op & 1u) == 0)
	{
						if ((op & 0x3eu) != 0)
							return true; // Non-zero even selectors are no-op.

						const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
						if (cycles == 0 || !EmitAddScaledCyclesToCpu(cycles))
							return false;

						return m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&COP0_UpdatePCCR)) &&
							   load_rt_low(HOST_TMP0) &&
							   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS,
								   static_cast<u16>(PERF_PCCR_OFFSET)) &&
							   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&COP0_DiagnosticPCCR));
	}

	{
						const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
						const bool pcr1 = (op & 2u) != 0;
						const size_t pcr_offset = pcr1 ? PERF_PCR1_OFFSET : PERF_PCR0_OFFSET;
						const size_t last_perf_offset = LAST_PERF_CYCLE_OFFSET + (pcr1 ? sizeof(u64) : 0);
						if (cycles == 0 ||
							!EmitAddScaledCyclesToCpu(cycles) ||
							!load_rt_low(HOST_TMP2))
						{
							return false;
						}

						return m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(pcr_offset)) &&
							   EmitStoreCpuRegsU64(last_perf_offset, HOST_TMP0, HOST_TMP1, HOST_TMP4);
	}
				default:
					return load_rt_low(HOST_TMP0) &&
						   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(rd)));
			}
		}

		bool BlockCompiler::EmitSetNextEventDelta4FromCurrentCycle()
		{
			// PCSX2 owner: R5900.cpp::cpuSetNextEventDelta(4). HOST_TMP0/1
			// must hold the current committed cycle from EmitAddScaledCyclesToCpu().
			constexpr u32 EVENT_DELTA = 4;
			if (!m_code.EmitAddImm8(HOST_TMP2, HOST_TMP0, EVENT_DELTA, true) ||
				!m_code.EmitAdcImm8(HOST_TMP3, HOST_TMP1, 0) ||
				!m_code.EmitLdrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(NEXT_EVENT_OFFSET)) ||
				!m_code.EmitSubReg(HOST_TMP4, HOST_TMP4, HOST_TMP0, true) ||
				!EmitCmpImm32OrReg(HOST_TMP4, EVENT_DELTA, HOST_TMP5))
			{
				return false;
			}

			const size_t keep_event = m_code.EmitBranchPlaceholder(VitaA32::Condition::LE);
			if (keep_event == static_cast<size_t>(-1))
				return false;

			if (!EmitStoreCpuRegsU64(NEXT_EVENT_OFFSET, HOST_TMP2, HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			return m_code.PatchBranch(keep_event, m_code.Size(), VitaA32::Condition::LE);
		}

		bool BlockCompiler::EmitEIEventExit(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit)
		{
			// PCSX2 owner: x86/iCOP0.cpp::recEI() must branch after
			// COP0.cpp::EI() so pending interrupts can be tested. Inline the
			// Status.EIE update and cpuSetNextEventDelta(4), but keep the event
			// tail instead of continuing through the block.
			constexpr u32 EI_ALLOWED_MASK = 0x00020006u; // Status._EDI | EXL | ERL
			constexpr u32 EI_KSU_MASK = 0x00000018u;
			constexpr u32 STATUS_EIE_SET_MASK = 0x00010000u;

			if (!event_exit || raw_cycles_through_instruction == 0)
				return false;

			// This event tail bypasses the normal block-exit flush.
			if (!EmitFlushDirtyGprPins())
				return false;

			const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
			if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
				!EmitStorePc(next_pc) ||
				!EmitAddScaledCyclesToCpu(cycles))
			{
				return false;
			}

			if (!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))) ||
				!EmitAndImm32OrReg(HOST_TMP4, HOST_TMP2, EI_ALLOWED_MASK, HOST_TMP3, true))
			{
				return false;
			}

			const size_t set_eie = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (set_eie == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitTstImm32(HOST_TMP2, EI_KSU_MASK))
			{
				return false;
			}

			const size_t skip_ei = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (skip_ei == static_cast<size_t>(-1))
				return false;

			const size_t set_target = m_code.Size();
			if (!m_code.PatchBranch(set_eie, set_target, VitaA32::Condition::NE) ||
				!EmitOrrImm32OrReg(HOST_TMP2, HOST_TMP2, STATUS_EIE_SET_MASK, HOST_TMP3) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))) ||
				!EmitSetNextEventDelta4FromCurrentCycle())
			{
				return false;
			}

			if (!m_code.PatchBranch(skip_ei, m_code.Size(), VitaA32::Condition::NE))
				return false;

			return EmitEventExitReturn(event_exit);
		}

		bool BlockCompiler::EmitTLBRInBlock()
		{
			// PCSX2 owners: COP0.cpp::TLBR(), x86/iCOP0.cpp::recTLBR().
			// TLBR reads the architectural TLB table only, so it can be emitted
			// directly; the normal block tail owns PC, cycle, and event testing.
			if (!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(0))) ||
				!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 0x3f) ||
				!EmitCmpImm32OrReg(HOST_TMP2, TLB_ENTRY_COUNT, HOST_TMP3))
			{
				return false;
			}

			const size_t invalid_index = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
			if (invalid_index == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP3, static_cast<u32>(reinterpret_cast<uptr>(&tlb[0]))) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP2, VitaA32::ShiftType::LSL, 4) ||
				!m_code.EmitAddReg(HOST_TMP3, HOST_TMP3, HOST_TMP4) ||
				!m_code.EmitLdrImm12(HOST_TMP4, HOST_TMP3, static_cast<u16>(TLB_PAGE_MASK_OFFSET)) ||
				!EmitAndImm32OrReg(HOST_TMP4, HOST_TMP4, TLB_PAGE_MASK_REGISTER_MASK, HOST_TMP5) ||
				!m_code.EmitStrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(5))) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP3, static_cast<u16>(TLB_ENTRY_HI_OFFSET)) ||
				!EmitOrrImm32OrReg(HOST_TMP5, HOST_TMP4, 0x1f00u, HOST_TMP5) ||
				!m_code.EmitMvnReg(HOST_TMP5, HOST_TMP5) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP5) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(10))) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP3, static_cast<u16>(TLB_ENTRY_LO0_OFFSET)) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP3, static_cast<u16>(TLB_ENTRY_LO1_OFFSET)) ||
				!m_code.EmitAndReg(HOST_TMP2, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 1) ||
				!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, TLB_TLBR_ENTRY_LO0_MASK, HOST_TMP5) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(2))) ||
				!EmitAndImm32OrReg(HOST_TMP1, HOST_TMP1, TLB_TLBR_ENTRY_LO1_MASK, HOST_TMP5) ||
				!m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(3))))
			{
				return false;
			}

			if (!m_code.PatchBranch(invalid_index, m_code.Size(), VitaA32::Condition::CS))
				return false;

			return true;
		}

		bool BlockCompiler::EmitTLBPInBlock()
		{
			// PCSX2 owners: COP0.cpp::TLBP(), x86/iCOP0.cpp::recTLBP().
			// Keep COP0.cpp's EntryHi32 bitfield view exactly: VPN2 is the
			// low 19 bits of EntryHi, while ASID is bits 24..31.
			// The operation has no cycle-dependent side effects, so the normal
			// block tail owns PC, cycle, and event testing.
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(10))) ||
				!EmitAndImm32OrReg(HOST_TMP3, HOST_TMP0, TLB_ENTRY_HI32_VPN2_MASK, HOST_TMP1) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSR, 24) ||
				!m_code.EmitMovImm32(HOST_TMP5, static_cast<u32>(reinterpret_cast<uptr>(&tlb[0]))) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0))
			{
				return false;
			}

			const size_t loop_start = m_code.Size();
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP5, static_cast<u16>(TLB_PAGE_MASK_OFFSET)) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSR, 13) ||
				!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, TLB_MASK_FIELD_MASK, HOST_TMP1) ||
				!m_code.EmitMvnReg(HOST_TMP0, HOST_TMP0) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP5, static_cast<u16>(TLB_ENTRY_HI_OFFSET)) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSR, 13) ||
				!m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP0) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, 13) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP3, HOST_TMP0) ||
				!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP0))
			{
				return false;
			}

			const size_t vpn_mismatch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (vpn_mismatch == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP5, static_cast<u16>(TLB_ENTRY_LO0_OFFSET)) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP5, static_cast<u16>(TLB_ENTRY_LO1_OFFSET)) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitAndImm8(HOST_TMP0, HOST_TMP0, 1, true))
			{
				return false;
			}

			const size_t global_match = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (global_match == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP5, static_cast<u16>(TLB_ENTRY_HI_OFFSET)) ||
				!m_code.EmitAndImm8(HOST_TMP0, HOST_TMP0, 0xff) ||
				!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP2))
			{
				return false;
			}

			const size_t asid_match = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (asid_match == static_cast<size_t>(-1))
				return false;

			const size_t next_entry = m_code.Size();
			if (!m_code.PatchBranch(vpn_mismatch, next_entry, VitaA32::Condition::NE) ||
				!m_code.EmitAddImm8(HOST_TMP5, HOST_TMP5, static_cast<u8>(TLB_ENTRY_SIZE)) ||
				!m_code.EmitAddImm8(HOST_TMP4, HOST_TMP4, 1) ||
				!EmitCmpImm32OrReg(HOST_TMP4, TLB_ENTRY_COUNT, HOST_TMP0))
			{
				return false;
			}

			const size_t continue_loop = m_code.EmitBranchPlaceholder(VitaA32::Condition::CC);
			if (continue_loop == static_cast<size_t>(-1))
				return false;

			if (!m_code.PatchBranch(continue_loop, loop_start, VitaA32::Condition::CC) ||
				!m_code.EmitMovImm32(HOST_TMP0, 0x80000000u) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(0))))
			{
				return false;
			}

			const size_t done = m_code.EmitBranchPlaceholder();
			if (done == static_cast<size_t>(-1))
				return false;

			const size_t match = m_code.Size();
			if (!m_code.PatchBranch(global_match, match, VitaA32::Condition::NE) ||
				!m_code.PatchBranch(asid_match, match, VitaA32::Condition::EQ) ||
				!m_code.EmitStrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(0))) ||
				!m_code.PatchBranch(done, m_code.Size()))
			{
				return false;
			}

			return true;
		}

		bool BlockCompiler::EmitERETEventExit(u32 op, u32 raw_cycles_through_instruction,
			const void* event_exit)
		{
			// PCSX2 owner: x86/iCOP0.cpp::recERET() branches after
			// COP0.cpp::ERET(). Inline the ERL/ErrorEPC versus EXL/EPC state
			// update and keep the required event-test tail.
			constexpr u32 STATUS_EXL_MASK = 0x00000002u;
			constexpr u32 STATUS_ERL_MASK = 0x00000004u;

			if (!event_exit || raw_cycles_through_instruction == 0)
				return false;

			// This event tail bypasses the normal block-exit flush.
			if (!EmitFlushDirtyGprPins())
				return false;

			const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
			if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
				!EmitAddScaledCyclesToCpu(cycles) ||
				!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))) ||
				!m_code.EmitTstImm32(HOST_TMP2, STATUS_ERL_MASK))
			{
				return false;
			}

			const size_t exl_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (exl_path == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitLdrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(30))) ||
				!m_code.EmitStrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(PC_OFFSET)) ||
				!EmitBicImm32OrReg(HOST_TMP2, HOST_TMP2, STATUS_ERL_MASK, HOST_TMP3) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))))
			{
				return false;
			}

			const size_t done = m_code.EmitBranchPlaceholder();
			if (done == static_cast<size_t>(-1))
				return false;

			const size_t exl_target = m_code.Size();
			if (!m_code.PatchBranch(exl_path, exl_target, VitaA32::Condition::EQ) ||
				!m_code.EmitLdrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(14))) ||
				!m_code.EmitStrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(PC_OFFSET)) ||
				!EmitBicImm32OrReg(HOST_TMP2, HOST_TMP2, STATUS_EXL_MASK, HOST_TMP3) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))))
			{
				return false;
			}

			if (!m_code.PatchBranch(done, m_code.Size()) ||
				!EmitSetNextEventDelta4FromCurrentCycle())
			{
				return false;
			}

			return EmitEventExitReturn(event_exit);
		}

		bool BlockCompiler::EmitDIDelayedStatusClear()
		{
			// PCSX2 owner: x86/iCOP0.cpp::recDI() inlines COP0.cpp::DI() after
			// recompiling the next instruction, so Status.EIE changes only after
			// that following instruction has observed the old Status value.
			constexpr u32 DI_ALLOWED_MASK = 0x00020006u; // Status._EDI | EXL | ERL
			constexpr u32 DI_KSU_MASK = 0x00000018u;
			constexpr u32 STATUS_EIE_MASK = 0x00010000u;

			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))) ||
				!EmitAndImm32OrReg(HOST_TMP2, HOST_TMP0, DI_ALLOWED_MASK, HOST_TMP1, true))
			{
				return false;
			}

			const size_t clear_eie = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (clear_eie == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitTstImm32(HOST_TMP0, DI_KSU_MASK))
			{
				return false;
			}

			const size_t done = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (done == static_cast<size_t>(-1))
				return false;

			const size_t clear_target = m_code.Size();
			if (!m_code.PatchBranch(clear_eie, clear_target, VitaA32::Condition::NE) ||
				!EmitBicImm32OrReg(HOST_TMP0, HOST_TMP0, STATUS_EIE_MASK, HOST_TMP1) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))))
			{
				return false;
			}

			return m_code.PatchBranch(done, m_code.Size(), VitaA32::Condition::NE);
		}

	bool BlockCompiler::EmitCOP1(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		if (IsFastCOP1MoveControl(op))
			return EmitCOP1MoveControlFast(op);
		if (IsFastCOP1ArithmeticOp(op))
			return EmitCOP1ArithmeticFast(op);
		if (IsFastCOP1DivSqrtOp(op))
			return EmitCOP1DivSqrtFast(op);
		if (IsFastCOP1AccumulatorOp(op))
			return EmitCOP1AccumulatorFast(op);
		if (IsFastCOP1ScalarWordOp(op))
			return EmitCOP1ScalarWordFast(op);
		if (IsFastCOP1CompareOp(op))
			return EmitCOP1CompareFast(op);
		if (IsFastCOP1ConvertWordOp(op))
			return EmitCOP1ConvertWordFast(op);
		if (IsFastCOP1ConvertSingleOp(op))
			return EmitCOP1ConvertSingleFast(op);

		// PCSX2 owner: x86/iFPU.cpp helper-calls FPU.cpp for arithmetic/control
		// fallbacks; the Vita native FPU arithmetic path will replace this
		// one-op event tail later.
		using namespace R5900::Interpreter::OpcodeImpl::COP1;
		const void* helper = nullptr;

		switch ((op >> 21) & 0x1f)
		{
			case 0x00: // MFC1, owned by FPU.cpp::MFC1().
				helper = reinterpret_cast<const void*>(&MFC1);
				break;
			case 0x02: // CFC1, owned by FPU.cpp::CFC1().
				helper = reinterpret_cast<const void*>(&CFC1);
				break;
			case 0x04: // MTC1, owned by FPU.cpp::MTC1().
				helper = reinterpret_cast<const void*>(&MTC1);
				break;
			case 0x06: // CTC1, owned by FPU.cpp::CTC1().
				helper = reinterpret_cast<const void*>(&CTC1);
				break;
			case 0x10: // COP1_S class, owned by R5900OpcodeTables.cpp::tbl_COP1_S.
				switch (op & 0x3f)
	{
					case 0x00: helper = reinterpret_cast<const void*>(&ADD_S); break;
					case 0x01: helper = reinterpret_cast<const void*>(&SUB_S); break;
					case 0x02: helper = reinterpret_cast<const void*>(&MUL_S); break;
					case 0x03: helper = reinterpret_cast<const void*>(&DIV_S); break;
					case 0x04: helper = reinterpret_cast<const void*>(&SQRT_S); break;
					case 0x05: helper = reinterpret_cast<const void*>(&ABS_S); break;
					case 0x06: helper = reinterpret_cast<const void*>(&MOV_S); break;
					case 0x07: helper = reinterpret_cast<const void*>(&NEG_S); break;
					case 0x16: helper = reinterpret_cast<const void*>(&RSQRT_S); break;
					case 0x18: helper = reinterpret_cast<const void*>(&ADDA_S); break;
					case 0x19: helper = reinterpret_cast<const void*>(&SUBA_S); break;
					case 0x1a: helper = reinterpret_cast<const void*>(&MULA_S); break;
					case 0x1c: helper = reinterpret_cast<const void*>(&MADD_S); break;
					case 0x1d: helper = reinterpret_cast<const void*>(&MSUB_S); break;
					case 0x1e: helper = reinterpret_cast<const void*>(&MADDA_S); break;
					case 0x1f: helper = reinterpret_cast<const void*>(&MSUBA_S); break;
					case 0x24: helper = reinterpret_cast<const void*>(&CVT_W); break;
					case 0x28: helper = reinterpret_cast<const void*>(&MAX_S); break;
					case 0x29: helper = reinterpret_cast<const void*>(&MIN_S); break;
					case 0x30: helper = reinterpret_cast<const void*>(&C_F); break;
					case 0x32: helper = reinterpret_cast<const void*>(&C_EQ); break;
					case 0x34: helper = reinterpret_cast<const void*>(&C_LT); break;
					case 0x36: helper = reinterpret_cast<const void*>(&C_LE); break;
					default:
						return false;
	}
				break;
			case 0x14: // COP1_W class, owned by R5900OpcodeTables.cpp::tbl_COP1_W.
				if ((op & 0x3f) == 0x20)
					helper = reinterpret_cast<const void*>(&CVT_S);
				else
					return false;
				break;
			default:
				return false;
		}

		return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction, helper, event_exit);
	}

	bool BlockCompiler::EmitCOP2(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		if (IsFastCOP2VectorTransfer(op))
			return EmitCOP2VectorTransferFast(op, pc + 4, raw_cycles_through_instruction, event_exit);
		if (IsFastCOP2ControlRead(op))
			return EmitCOP2ControlReadFast(op, pc + 4, raw_cycles_through_instruction, event_exit);
		if (IsFastCOP2ControlWrite(op))
			return EmitCOP2ControlWriteFast(op, pc + 4, raw_cycles_through_instruction, event_exit);
		if (IsFastCOP2MacroInBlock(op))
			return EmitCOP2MacroFast(op, pc + 4, raw_cycles_through_instruction, event_exit);

#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		return false;
#else
		// PCSX2 owners: R5900OpcodeImpl.cpp::COP2(), COP2.cpp, VU0.cpp, and
		// VUops.cpp. Non-branch COP2 forms exit through the same interpreter
		// dispatcher so VU0 sync, VCALLMS, flags, and transfer side effects stay
		// PCSX2-owned while the full machine is brought up.
		return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction,
			reinterpret_cast<const void*>(&R5900::Interpreter::OpcodeImpl::COP2), event_exit);
#endif
	}

	bool BlockCompiler::EmitCOP2IdleBranch(size_t* vu0_idle)
	{
		// PCSX2 owner: VU0.cpp::vu0Sync(). The native in-block path is valid
		// only while VU0 is idle; if a microprogram is running, the caller emits
		// the previous sync-and-exit path at this instruction's EE cycle.
		if (!vu0_idle)
			return false;

		if (!EmitVu0ViAddress(HOST_TMP0, VU0_REG_VPU_STAT) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0) ||
			!m_code.EmitTstImm32(HOST_TMP1, 1))
		{
			return false;
		}

		*vu0_idle = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		return *vu0_idle != static_cast<size_t>(-1);
	}

	bool BlockCompiler::EmitCOP2VectorTransferBody(u32 op)
	{
		const unsigned rt = RT(op);
		const unsigned fs = RD(op);
		constexpr unsigned NEON_VALUE = 0;

		switch ((op >> 21) & 0x1f)
		{
			case 0x01: // QMFC2
				if (rt == 0)
					break;

				if (fs == 0)
	{
					if (!EmitVu0Vf0ConstantQ(NEON_VALUE, HOST_TMP1) ||
						!EmitStoreGprQ128(rt, NEON_VALUE, HOST_TMP1))
	{
						return false;
	}
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuCop2Vf0ConstantTransferFastPaths++;
#endif
					break;
	}

				if (!EmitVu0VfAddress(HOST_TMP0, fs) ||
					!m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP0) ||
					!EmitStoreGprQ128(rt, NEON_VALUE, HOST_TMP1))
	{
					return false;
	}
				break;
			case 0x05: // QMTC2
			{
				if (fs == 0)
					break;

				if (!EmitVu0VfAddress(HOST_TMP1, fs))
	{
					return false;
	}

				if (rt == 0)
	{
					// PCSX2 owner: VU0.cpp::QMTC2() reads raw GPR[0], not the
					// architectural zero register. The executor keeps this flag
					// exact; use it to avoid a 128-bit cpuRegs load on the common
					// raw-zero path and fall back to the raw backing slot otherwise.
					InvalidateGprQCacheForQreg(NEON_VALUE);
					if (!EmitLoadRawGpr0KnownZeroFlag(HOST_TMP0) ||
						!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL, 0, true))
	{
						return false;
	}

					const size_t raw_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
					if (raw_fallback == static_cast<size_t>(-1))
						return false;

					if (!m_code.EmitVeorQ(NEON_VALUE, NEON_VALUE, NEON_VALUE) ||
						!m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP1))
	{
						return false;
	}
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuCop2RawGpr0Qmtc2ZeroFastPaths++;
#endif

					const size_t zero_done = m_code.EmitBranchPlaceholder();
					if (zero_done == static_cast<size_t>(-1))
						return false;

					const size_t raw_fallback_target = m_code.Size();
					if (!m_code.PatchBranch(raw_fallback, raw_fallback_target, VitaA32::Condition::EQ) ||
						!EmitLoadCpuRegsQ128(GprOffset(0), NEON_VALUE, HOST_TMP0) ||
						!m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP1) ||
						!m_code.PatchBranch(zero_done, m_code.Size()))
	{
						return false;
	}
					break;
	}

				const int cached_qreg = FindGprQCache(rt);
				if (cached_qreg >= 0)
	{
					if (!m_code.EmitVst1Q32Aligned(static_cast<unsigned>(cached_qreg), HOST_TMP1))
						return false;
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuCop2Qmtc2QCacheFastPaths++;
					g_qemuCop2Qmtc2QCacheDirectStores++;
#endif
					break;
	}

				unsigned value_qreg = NEON_VALUE;
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (!IsGprQCacheQregResident(qreg))
	{
						value_qreg = qreg;
						break;
	}
	}

				if (!EmitLoadGprQ128(rt, value_qreg, HOST_TMP0) ||
					!m_code.EmitVst1Q32Aligned(value_qreg, HOST_TMP1))
	{
					return false;
	}
				break;
			}
			default:
				return false;
		}

		return true;
	}

	bool BlockCompiler::EmitCOP2ControlReadBody(u32 op)
	{
		// PCSX2 owners: VU0.cpp::CFC2() and x86/microVU_Macro.inl::recCFC2().
		// REG_R only writes the low GPR word, while other VI registers
		// sign-extend into the low 64-bit GPR half.
		const unsigned rt = RT(op);
		const unsigned fs = RD(op);

		if (rt != 0)
		{
			if (!EmitVu0ViAddress(HOST_TMP0, fs) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0))
			{
				return false;
			}

			if (fs == VU0_REG_R)
			{
				if (!m_code.EmitUbfx(HOST_TMP1, HOST_TMP1, 0, 23) ||
					!EmitStoreGprLowPreserveHigh(rt, HOST_TMP1))
	{
					return false;
	}
			}
			else if (!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::ASR, 31) ||
					 !EmitStoreGpr64(rt, HOST_TMP1, HOST_TMP2))
			{
				return false;
			}
		}

		return true;
	}

	bool BlockCompiler::EmitCOP2ControlWriteBody(u32 op)
	{
		// PCSX2 owners: VU0.cpp::CTC2() and x86/microVU_Macro.inl::recCTC2().
		// CMSAR1 is emitted as a separate cycle-committing event tail because
		// it starts VU1 microcode; this fall-through body is no-op, masked, raw
		// VI low-word writes, or FBRST's reset-bit calls followed by the masked
		// mode-bit store.
		const unsigned rt = RT(op);
		const unsigned fs = RD(op);

		switch (fs)
		{
			case 0:
			case VU0_REG_MAC_FLAG:
			case VU0_REG_TPC:
			case VU0_REG_VPU_STAT:
				break;

			case VU0_REG_R:
				if (!EmitLoadGprLowRawZero(rt, HOST_TMP1) ||
					!m_code.EmitUbfx(HOST_TMP1, HOST_TMP1, 0, 23) ||
					!EmitOrrImm32OrReg(HOST_TMP1, HOST_TMP1, 0x3f800000u, HOST_TMP2) ||
					!EmitVu0ViAddress(HOST_TMP0, fs) ||
					!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0))
	{
					return false;
	}
				break;

			case VU0_REG_FBRST:
			{
				if (!EmitLoadGprLowRawZero(rt, HOST_TMP5) ||
					!m_code.EmitTstImm32(HOST_TMP5, 0x2u))
	{
					return false;
	}

				size_t skip_vu0_reset = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
				if (skip_vu0_reset == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu0ResetRegs)) ||
					!m_code.PatchBranch(skip_vu0_reset, m_code.Size(), VitaA32::Condition::EQ) ||
					!m_code.EmitTstImm32(HOST_TMP5, 0x200u))
	{
					return false;
	}

				size_t skip_vu1_reset = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
				if (skip_vu1_reset == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu1ResetRegs)) ||
					!m_code.PatchBranch(skip_vu1_reset, m_code.Size(), VitaA32::Condition::EQ) ||
					!EmitAndImm32OrReg(HOST_TMP5, HOST_TMP5, 0x0c0cu, HOST_TMP1) ||
					!EmitVu0ViAddress(HOST_TMP0, fs) ||
					!m_code.EmitStrImm12(HOST_TMP5, HOST_TMP0, 0))
	{
					return false;
	}
				break;
			}

			case VU0_REG_CLIP_FLAG:
				if (!EmitLoadGprLowRawZero(rt, HOST_TMP1) ||
					!EmitVu0ClipflagAddress(HOST_TMP0) ||
					!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0) ||
					!EmitVu0ViAddress(HOST_TMP0, fs) ||
					!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0))
	{
					return false;
	}
				break;

			default:
				if (!EmitLoadGprLowRawZero(rt, HOST_TMP1) ||
					!EmitVu0ViAddress(HOST_TMP0, fs) ||
					!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0))
	{
					return false;
	}
				break;
		}

		return true;
	}

	bool BlockCompiler::EmitCOP2MacroCodeWrite(u32 op)
	{
		if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)))
		{
			return false;
		}

#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		return true;
#else
		return EmitVu0RegisterAddress(HOST_TMP1, VU0_CODE_OFFSET) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_TMP1, 0);
#endif
	}

	bool BlockCompiler::EmitCOP2MacroCopySelectedLanes(unsigned mask, unsigned source_address_reg,
		unsigned dest_address_reg, unsigned temp_qreg)
	{
		mask &= 0x0f;
		if (mask == 0)
			return true;
		if (mask == 0x0f)
			return m_code.EmitVld1Q32Aligned(temp_qreg, source_address_reg) &&
				   m_code.EmitVst1Q32Aligned(temp_qreg, dest_address_reg);
		if (temp_qreg >= 4)
			return false;

		const auto emit_lane = [&](unsigned lane) {
			const u16 offset = static_cast<u16>(lane * sizeof(u32));
			const unsigned sreg = temp_qreg * 4 + lane;
			return m_code.EmitVldrSImm(sreg, source_address_reg, offset) &&
				   m_code.EmitVstrSImm(sreg, dest_address_reg, offset);
		};

		if ((mask & 0x8) && !emit_lane(0))
			return false;
		if ((mask & 0x4) && !emit_lane(1))
			return false;
		if ((mask & 0x2) && !emit_lane(2))
			return false;
		if ((mask & 0x1) && !emit_lane(3))
			return false;

		return true;
	}

	bool BlockCompiler::EmitCOP2MacroStoreSelectedLanes(unsigned mask, unsigned value_qreg, unsigned address_reg)
	{
		mask &= 0x0f;
		if (mask == 0)
			return true;
		if (mask == 0x0f)
			return m_code.EmitVst1Q32Aligned(value_qreg, address_reg);
		if (value_qreg >= 4)
			return false;

		const auto emit_lane = [&](unsigned lane) {
			return m_code.EmitVstrSImm(value_qreg * 4 + lane, address_reg,
				static_cast<u16>(lane * sizeof(u32)));
		};

		if ((mask & 0x8) && !emit_lane(0))
			return false;
		if ((mask & 0x4) && !emit_lane(1))
			return false;
		if ((mask & 0x2) && !emit_lane(2))
			return false;
		if ((mask & 0x1) && !emit_lane(3))
			return false;

		return true;
	}

	bool BlockCompiler::EmitCOP2MacroStoreVfSelectedLanes(unsigned vf_reg, unsigned mask, unsigned value_qreg,
		unsigned address_reg)
	{
		mask &= 0x0f;
		if (vf_reg == 0 || mask == 0)
			return true;
		return EmitVu0VfAddress(address_reg, vf_reg) &&
			   EmitCOP2MacroStoreSelectedLanes(mask, value_qreg, address_reg);
	}

	bool BlockCompiler::EmitCOP2MacroBody(u32 op)
	{
		// PCSX2 owners: VU0.cpp::COP2_SPECIAL(), VUops.cpp macro helpers, and
		// x86/microVU_Macro.inl rec* macro lowerings. This body is emitted only
		// on the idle-VU0 path; running VU0 exits through the COP2 helper so
		// _vu0FinishMicro() still owns the interlock and cycle side effects.
		if (DecodeCop2MacroArithmetic(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroArithmeticBody(op);
		if (DecodeCop2MacroMinMax(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroMinMaxBody(op);
		if (DecodeCop2MacroMove(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroMoveBody(op);
		if (DecodeCop2MacroVi(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroViBody(op);
		if (DecodeCop2MacroViTransfer(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroViTransferBody(op);
		if (DecodeCop2MacroRandom(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroRandomBody(op);
		if (DecodeCop2MacroIndexedViMemory(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroIndexedViMemoryBody(op);
		if (DecodeCop2MacroIndexedVectorMemory(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroIndexedVectorMemoryBody(op);
		if (IsCop2MacroClip(op))
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroClipBody(op);
		if (DecodeCop2MacroItof(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroItofBody(op);
		if (DecodeCop2MacroFtoi(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroFtoiBody(op);
		if (DecodeCop2MacroFdiv(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroFdivBody(op);

		const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
		if (!EmitCOP2MacroCodeWrite(op))
			return false;

		if (special2_index == 0x2f) // VNOP
			return true;

		if (special2_index != 0x1d) // VABS
			return false;

		const unsigned ft = RT(op);
		if (ft == 0)
			return true;

		const unsigned fs = RD(op);
		const unsigned mask = (op >> 21) & 0x0f;
		if (mask == 0)
			return true;

		constexpr unsigned NEON_VALUE = 0;
		constexpr unsigned NEON_SIGN_MASK = 1;
		return EmitVu0VfAddress(HOST_TMP0, fs) &&
			   m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP0) &&
			   m_code.EmitMovImm32(HOST_TMP2, 0x7fffffffu) &&
			   m_code.EmitVdupI32QFromCore(NEON_SIGN_MASK, HOST_TMP2) &&
			   m_code.EmitVandQ(NEON_VALUE, NEON_VALUE, NEON_SIGN_MASK) &&
			   EmitCOP2MacroStoreVfSelectedLanes(ft, mask, NEON_VALUE, HOST_TMP1);
	}

	bool BlockCompiler::EmitCOP2MacroArithmeticBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuADD/_vuADDi/_vuSUB/_vuMUL/_vuMADD/
		// _vuMSUB/_vuOPMULA/_vuOPMSUB plus their broadcast/ACC variants,
		// VUops.cpp::vuADD_TriAceHack(), VUflags.cpp::VU_MAC*_UPDATE(), and
		// VUops.cpp::SYNCMSFLAGS().
		const Cop2MacroArithmeticOp arithmetic = DecodeCop2MacroArithmetic(op);
		if (!arithmetic.valid)
			return false;

#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		const bool vu0_overflow_clamp = true;
#else
		const bool vu0_overflow_clamp = CHECK_VU_OVERFLOW(0);
#endif
		const unsigned fs = RD(op);
		const unsigned ft = RT(op);
		const unsigned fd = SA(op);
		const unsigned mask = (op >> 21) & 0x0f;
		constexpr unsigned VFP_FS_S0 = 0;
		constexpr unsigned VFP_FT_S1 = 1;
		constexpr unsigned VFP_RESULT_S2 = 2;
		constexpr unsigned VFP_BROADCAST_S3 = 3;
		constexpr unsigned VFP_ACC_S4 = 4;
		// NEON-quad input-normalization path. The operand quads live in Q0-Q2
		// (S0-S11) so the per-lane scalar arithmetic can read normalized lanes
		// directly; the result uses S12 (Q3), clear of the quads. The vuDouble()
		// bit-select scratch uses Q8-Q15 (D16-D31, no single-precision alias), so
		// it never collides with the S0-S11 operands.
		constexpr unsigned QUAD_ACC = 0;             // Q0 -> S0-S3
		constexpr unsigned QUAD_FS = 1;              // Q1 -> S4-S7
		constexpr unsigned QUAD_FT = 2;              // Q2 -> S8-S11
		constexpr unsigned VFP_QUAD_RESULT_S12 = 12; // Q3 lane 0
		constexpr unsigned NQ_EXP = 8;
		constexpr unsigned NQ_SIGN = 9;
		constexpr unsigned NQ_MAXF = 10;
		constexpr unsigned NQ_ZERO = 11;
		constexpr unsigned NQ_EXPV = 12;
		constexpr unsigned NQ_SIGNV = 13;
		constexpr unsigned NQ_TMP = 14;
		constexpr unsigned NQ_MASK = 15;
		const bool is_outer_product = arithmetic.kind == Cop2MacroArithmeticKind::OpMula ||
									  arithmetic.kind == Cop2MacroArithmeticKind::OpMSub;
		const bool uses_acc_source = arithmetic.kind == Cop2MacroArithmeticKind::MAdd ||
									 arithmetic.kind == Cop2MacroArithmeticKind::MSub ||
									 arithmetic.kind == Cop2MacroArithmeticKind::OpMSub;
		const bool use_addi_triace_hack = arithmetic.addi_triace_hack && CHECK_VUADDSUBHACK;

		const auto emit_normalize_vu_float_word = [&](unsigned reg) {
			if (!EmitAndImm32OrReg(HOST_TMP3, reg, FPU_FLOAT_EXPONENT_MASK, HOST_TMP5) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t exponent_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (exponent_nonzero == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP5))
				return false;

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t exponent_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(exponent_nonzero, exponent_nonzero_target, VitaA32::Condition::NE))
				return false;

			size_t done_from_finite = static_cast<size_t>(-1);
			if (vu0_overflow_clamp)
			{
				if (!EmitCmpImm32OrReg(HOST_TMP3, FPU_FLOAT_EXPONENT_MASK, HOST_TMP5))
					return false;

				done_from_finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
				if (done_from_finite == static_cast<size_t>(-1))
					return false;

				if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP5) ||
					!EmitOrrImm32OrReg(reg, reg, FPU_FLOAT_MAX_FINITE, HOST_TMP5))
	{
					return false;
	}
			}

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_from_zero, done_target) &&
				   (done_from_finite == static_cast<size_t>(-1) ||
					   m_code.PatchBranch(done_from_finite, done_target, VitaA32::Condition::NE));
		};

		const auto emit_load_vf_lane = [&](unsigned host_reg, unsigned vf_reg, unsigned lane) {
			return EmitVu0VfAddress(host_reg, vf_reg) &&
				   m_code.EmitLdrImm12(host_reg, host_reg, static_cast<u16>(lane * sizeof(u32)));
		};

		const auto emit_prepare_broadcast_operand = [&]() {
			switch (arithmetic.operand)
			{
				case Cop2MacroArithmeticOperand::Vector:
					return true;
				case Cop2MacroArithmeticOperand::BroadcastLane:
					return emit_load_vf_lane(HOST_TMP1, ft, arithmetic.broadcast_lane) &&
						   m_code.EmitVmovCoreToS(VFP_BROADCAST_S3, HOST_TMP1);
				case Cop2MacroArithmeticOperand::ImmediateI:
					return EmitVu0ViAddress(HOST_TMP1, VU0_REG_I) &&
						   m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP1, 0) &&
						   m_code.EmitVmovCoreToS(VFP_BROADCAST_S3, HOST_TMP1);
				case Cop2MacroArithmeticOperand::ImmediateQ:
					return EmitVu0ViAddress(HOST_TMP1, VU0_REG_Q) &&
						   m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP1, 0) &&
						   m_code.EmitVmovCoreToS(VFP_BROADCAST_S3, HOST_TMP1);
			}
			return false;
		};

		const auto emit_load_operand_lane_raw = [&](unsigned lane) {
			if (arithmetic.operand == Cop2MacroArithmeticOperand::Vector)
			{
				return emit_load_vf_lane(HOST_TMP1, ft, lane);
			}

			return m_code.EmitVmovSToCore(HOST_TMP1, VFP_BROADCAST_S3);
		};

		const auto emit_apply_triace_add_hack = [&]() {
			if (!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSR, 23) ||
				!EmitAndImm32OrReg(HOST_TMP2, HOST_TMP2, 0xffu, HOST_TMP5) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP1, VitaA32::ShiftType::LSR, 23) ||
				!EmitAndImm32OrReg(HOST_TMP3, HOST_TMP3, 0xffu, HOST_TMP5) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP3))
			{
				return false;
			}

			if (!m_code.EmitCmpImm32(HOST_TMP2, 25))
				return false;

			const size_t keep_b = m_code.EmitBranchPlaceholder(VitaA32::Condition::LT);
			if (keep_b == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP5))
				return false;

			if (!m_code.PatchBranch(keep_b, m_code.Size(), VitaA32::Condition::LT) ||
				!EmitCmpImm32OrReg(HOST_TMP2, static_cast<u32>(-25), HOST_TMP5))
			{
				return false;
			}

			const size_t keep_a = m_code.EmitBranchPlaceholder(VitaA32::Condition::GT);
			if (keep_a == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP5))
				return false;

			return m_code.PatchBranch(keep_a, m_code.Size(), VitaA32::Condition::GT);
		};

		const auto emit_clear_mac_lane = [&](unsigned lane) {
			const unsigned shift = 3 - lane;
			return EmitBicImm32OrReg(HOST_TMP4, HOST_TMP4, 0x1111u << shift, HOST_TMP5);
		};

		const auto emit_update_mac_lane = [&](unsigned lane) {
			const unsigned shift = 3 - lane;
			const u32 sign_flag = 0x0010u << shift;
			const u32 zero_clear = 0x1100u << shift;
			const u32 zero_set = 0x0001u << shift;
			const u32 denormal_clear = 0x1000u << shift;
			const u32 denormal_set = 0x0101u << shift;
			const u32 overflow_clear = 0x0101u << shift;
			const u32 overflow_set = 0x1000u << shift;
			const u32 finite_clear = 0x1101u << shift;

			if (!EmitBicImm32OrReg(HOST_TMP4, HOST_TMP4, sign_flag, HOST_TMP5) ||
				!m_code.EmitTstImm32(HOST_TMP0, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!m_code.EmitMovImm8(HOST_TMP2, static_cast<u8>(sign_flag), VitaA32::Condition::NE) ||
				!m_code.EmitOrrReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
				!EmitBicImm32OrReg(HOST_TMP2, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP5) ||
				!m_code.EmitCmpImm32(HOST_TMP2, 0))
			{
				return false;
			}

			const size_t nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (nonzero == static_cast<size_t>(-1))
				return false;

			if (!EmitBicImm32OrReg(HOST_TMP4, HOST_TMP4, zero_clear, HOST_TMP5) ||
				!EmitOrrImm32OrReg(HOST_TMP4, HOST_TMP4, zero_set, HOST_TMP5))
			{
				return false;
			}

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(nonzero, nonzero_target, VitaA32::Condition::NE) ||
				!EmitAndImm32OrReg(HOST_TMP2, HOST_TMP0, FPU_FLOAT_EXPONENT_MASK, HOST_TMP5) ||
				!m_code.EmitCmpImm32(HOST_TMP2, 0))
			{
				return false;
			}

			const size_t exponent_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (exponent_nonzero == static_cast<size_t>(-1))
				return false;

			if (!EmitBicImm32OrReg(HOST_TMP4, HOST_TMP4, denormal_clear, HOST_TMP5) ||
				!EmitOrrImm32OrReg(HOST_TMP4, HOST_TMP4, denormal_set, HOST_TMP5) ||
				!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP5))
			{
				return false;
			}

			const size_t done_from_denormal = m_code.EmitBranchPlaceholder();
			if (done_from_denormal == static_cast<size_t>(-1))
				return false;

			const size_t exponent_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(exponent_nonzero, exponent_nonzero_target, VitaA32::Condition::NE) ||
				!EmitCmpImm32OrReg(HOST_TMP2, FPU_FLOAT_EXPONENT_MASK, HOST_TMP5))
			{
				return false;
			}

			const size_t finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (finite == static_cast<size_t>(-1))
				return false;

			if (!EmitBicImm32OrReg(HOST_TMP4, HOST_TMP4, overflow_clear, HOST_TMP5) ||
				!EmitOrrImm32OrReg(HOST_TMP4, HOST_TMP4, overflow_set, HOST_TMP5))
			{
				return false;
			}

			if (vu0_overflow_clamp &&
				(!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP5) ||
				 !EmitOrrImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_MAX_FINITE, HOST_TMP5)))
			{
				return false;
			}

			const size_t done_from_overflow = m_code.EmitBranchPlaceholder();
			if (done_from_overflow == static_cast<size_t>(-1))
				return false;

			const size_t finite_target = m_code.Size();
			if (!m_code.PatchBranch(finite, finite_target, VitaA32::Condition::NE) ||
				!EmitBicImm32OrReg(HOST_TMP4, HOST_TMP4, finite_clear, HOST_TMP5))
			{
				return false;
			}

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_from_zero, done_target) &&
				   m_code.PatchBranch(done_from_denormal, done_target) &&
				   m_code.PatchBranch(done_from_overflow, done_target);
		};

		const auto emit_store_result = [&](unsigned lane) {
			if (arithmetic.acc_destination)
			{
				return EmitVu0RegisterAddress(HOST_TMP2, VU0_ACC_OFFSET) &&
					   m_code.EmitStrImm12(HOST_TMP0, HOST_TMP2, static_cast<u16>(lane * sizeof(u32)));
			}

			if (fd == 0)
				return true;

			return EmitVu0VfAddress(HOST_TMP2, fd) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_TMP2, static_cast<u16>(lane * sizeof(u32)));
		};

		const auto emit_sync_msflags = [&]() {
			const auto emit_status_bit = [&](u32 mac_mask, u8 status_bit) {
				return m_code.EmitTstImm32(HOST_TMP4, mac_mask) &&
					   m_code.EmitMovImm8(HOST_TMP2, 0) &&
					   m_code.EmitMovImm8(HOST_TMP2, status_bit, VitaA32::Condition::NE) &&
					   m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2);
			};

			if (!m_code.EmitMovImm8(HOST_TMP0, 0) ||
				!emit_status_bit(0x000fu, 0x1) ||
				!emit_status_bit(0x00f0u, 0x2) ||
				!emit_status_bit(0x0f00u, 0x4) ||
				!emit_status_bit(0xf000u, 0x8) ||
				!EmitVu0RegisterAddress(HOST_TMP1, VU0_MACFLAG_OFFSET) ||
				!m_code.EmitStrImm12(HOST_TMP4, HOST_TMP1, 0) ||
				!EmitVu0ViAddress(HOST_TMP1, VU0_REG_MAC_FLAG) ||
				!m_code.EmitStrImm12(HOST_TMP4, HOST_TMP1, 0) ||
				!EmitVu0RegisterAddress(HOST_TMP1, VU0_STATUSFLAG_OFFSET) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_TMP1, 0) ||
				!EmitVu0ViAddress(HOST_TMP1, VU0_REG_STATUS_FLAG) ||
				!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP1, 0) ||
				!EmitAndImm32OrReg(HOST_TMP2, HOST_TMP2, 0x0fc0u, HOST_TMP5) ||
				!m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP0) ||
				!m_code.EmitOrrRegShiftImm(HOST_TMP2, HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 6) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_TMP1, 0))
			{
				return false;
			}
			return true;
		};

		// Preloads the operand quad (Q2 / S8-S11) for the NEON-quad path. Mirrors
		// emit_prepare_broadcast_operand(): vector forms load all four lanes, the
		// broadcast/immediate forms replicate a single word across the quad.
		const auto emit_prepare_operand_quad = [&]() {
			switch (arithmetic.operand)
			{
				case Cop2MacroArithmeticOperand::Vector:
					return EmitVu0VfAddress(HOST_TMP0, ft) &&
						   m_code.EmitVld1Q32Aligned(QUAD_FT, HOST_TMP0);
				case Cop2MacroArithmeticOperand::BroadcastLane:
					return emit_load_vf_lane(HOST_TMP1, ft, arithmetic.broadcast_lane) &&
						   m_code.EmitVdupI32QFromCore(QUAD_FT, HOST_TMP1);
				case Cop2MacroArithmeticOperand::ImmediateI:
					return EmitVu0ViAddress(HOST_TMP1, VU0_REG_I) &&
						   m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP1, 0) &&
						   m_code.EmitVdupI32QFromCore(QUAD_FT, HOST_TMP1);
				case Cop2MacroArithmeticOperand::ImmediateQ:
					return EmitVu0ViAddress(HOST_TMP1, VU0_REG_Q) &&
						   m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP1, 0) &&
						   m_code.EmitVdupI32QFromCore(QUAD_FT, HOST_TMP1);
			}
			return false;
		};

		// Materializes the vuDouble() bit-select constants into Q8-Q11. Clobbers
		// HOST_TMP1, which is free before the lane loop.
		const auto emit_materialize_norm_consts = [&]() {
			if (!m_code.EmitMovImm32(HOST_TMP1, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitVdupI32QFromCore(NQ_EXP, HOST_TMP1) ||
				!m_code.EmitMovImm32(HOST_TMP1, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitVdupI32QFromCore(NQ_SIGN, HOST_TMP1) ||
				!m_code.EmitVeorQ(NQ_ZERO, NQ_ZERO, NQ_ZERO))
			{
				return false;
			}
			if (!vu0_overflow_clamp)
				return true;
			return m_code.EmitMovImm32(HOST_TMP1, FPU_FLOAT_MAX_FINITE) &&
				   m_code.EmitVdupI32QFromCore(NQ_MAXF, HOST_TMP1);
		};

		// Materializes the constants once per block. Physical Q8-Q11 are exclusive
		// to these persistent constants. Physical Q12-Q15 are normalize scratch in
		// COP2 macro blocks and back logical Q4-Q7 in qcache blocks; the qcache
		// classifier excludes macro arithmetic, so those roles never overlap.
		// m_cop2_norm_consts_ready is reset in BeginBlock(), so the first COP2 op
		// in every block always materializes.
		const auto emit_ensure_norm_consts = [&]() {
			if (m_cop2_norm_consts_ready)
				return true;
			if (!emit_materialize_norm_consts())
				return false;
			m_cop2_norm_consts_ready = true;
			return true;
		};

		// NEON-quad vuDouble() over all four lanes of vq. Bit-identical to the
		// scalar emit_normalize_vu_float_word() but branchless and NEON-only:
		// denormals (exp 0) flush to signed zero; with the overflow clamp,
		// inf/NaN (exp 0xff) become signed max finite. Requires the constants.
		const auto emit_normalize_quad = [&](unsigned vq) {
			if (!m_code.EmitVandQ(NQ_EXPV, vq, NQ_EXP) ||
				!m_code.EmitVandQ(NQ_SIGNV, vq, NQ_SIGN) ||
				!m_code.EmitVceqI32Q(NQ_MASK, NQ_EXPV, NQ_ZERO) ||
				!m_code.EmitVeorQ(NQ_TMP, NQ_SIGNV, vq) ||
				!m_code.EmitVandQ(NQ_TMP, NQ_TMP, NQ_MASK) ||
				!m_code.EmitVeorQ(vq, vq, NQ_TMP))
			{
				return false;
			}
			if (!vu0_overflow_clamp)
				return true;
			return m_code.EmitVceqI32Q(NQ_MASK, NQ_EXPV, NQ_EXP) &&
				   m_code.EmitVorrQ(NQ_TMP, NQ_SIGNV, NQ_MAXF) &&
				   m_code.EmitVeorQ(NQ_TMP, NQ_TMP, vq) &&
				   m_code.EmitVandQ(NQ_TMP, NQ_TMP, NQ_MASK) &&
				   m_code.EmitVeorQ(vq, vq, NQ_TMP);
		};

		if (!EmitVu0RegisterAddress(HOST_TMP2, VU0_MACFLAG_OFFSET) ||
			!m_code.EmitLdrImm12(HOST_TMP4, HOST_TMP2, 0))
		{
			return false;
		}

		// The TriAce add hack does a per-lane exponent compare that resists
		// vectorization, and the outer product has cross-lane fd==fs/ft store
		// hazards; both keep the scalar loop. Everything else preloads and
		// NEON-normalizes fs/ft/ACC once, then runs scalar VFP arithmetic per
		// active lane against the pre-normalized quad lanes.
		const bool use_quad = !use_addi_triace_hack && !is_outer_product;
		if (use_quad)
		{
			if (!EmitVu0VfAddress(HOST_TMP0, fs) ||
				!m_code.EmitVld1Q32Aligned(QUAD_FS, HOST_TMP0) ||
				!emit_prepare_operand_quad() ||
				(uses_acc_source &&
					(!EmitVu0RegisterAddress(HOST_TMP0, VU0_ACC_OFFSET) ||
						!m_code.EmitVld1Q32Aligned(QUAD_ACC, HOST_TMP0))) ||
				!emit_ensure_norm_consts() ||
				!emit_normalize_quad(QUAD_FS) ||
				!emit_normalize_quad(QUAD_FT) ||
				(uses_acc_source && !emit_normalize_quad(QUAD_ACC)))
			{
				return false;
			}

			for (unsigned lane = 0; lane < 4; lane++)
			{
				const unsigned lane_mask = 1u << (3 - lane);
				if ((mask & lane_mask) == 0)
	{
					if (!emit_clear_mac_lane(lane))
						return false;
					continue;
	}

				const unsigned fs_s = QUAD_FS * 4 + lane;
				const unsigned ft_s = QUAD_FT * 4 + lane;
				const unsigned acc_s = QUAD_ACC * 4 + lane;
				switch (arithmetic.kind)
	{
					case Cop2MacroArithmeticKind::Add:
						if (!m_code.EmitVaddF32(VFP_QUAD_RESULT_S12, fs_s, ft_s))
							return false;
						break;
					case Cop2MacroArithmeticKind::Sub:
						if (!m_code.EmitVsubF32(VFP_QUAD_RESULT_S12, fs_s, ft_s))
							return false;
						break;
					case Cop2MacroArithmeticKind::Mul:
					case Cop2MacroArithmeticKind::OpMula:
						if (!m_code.EmitVmulF32(VFP_QUAD_RESULT_S12, fs_s, ft_s))
							return false;
						break;
					case Cop2MacroArithmeticKind::MAdd:
						if (!m_code.EmitVmulF32(VFP_QUAD_RESULT_S12, fs_s, ft_s) ||
							!m_code.EmitVaddF32(VFP_QUAD_RESULT_S12, acc_s, VFP_QUAD_RESULT_S12))
						{
							return false;
						}
						break;
					case Cop2MacroArithmeticKind::MSub:
					case Cop2MacroArithmeticKind::OpMSub:
						if (!m_code.EmitVmulF32(VFP_QUAD_RESULT_S12, fs_s, ft_s) ||
							!m_code.EmitVsubF32(VFP_QUAD_RESULT_S12, acc_s, VFP_QUAD_RESULT_S12))
						{
							return false;
						}
						break;
	}

				if (!m_code.EmitVmovSToCore(HOST_TMP0, VFP_QUAD_RESULT_S12) ||
					!emit_update_mac_lane(lane) ||
					!emit_store_result(lane))
	{
					return false;
	}
			}

			return emit_sync_msflags();
		}

		if (is_outer_product)
		{
			// VOPMULA/VOPMSUB. The reference _vuOPMULA/_vuOPMSUB snapshot fs/ft
			// (and ACC for OPMSUB) into locals before writing the destination, so
			// fd == fs / fd == ft aliases observe the original operands. Preload
			// them as NEON quads before any store to keep that ordering (the old
			// per-lane scalar loop read fs/ft interleaved with fd stores and so
			// diverged for those aliases), normalize with the shared quad path,
			// arrange the {fs.y*ft.z, fs.z*ft.x, fs.x*ft.y} cross product with
			// cheap S-register moves (no ARM<->NEON transfers), and keep the qword
			// multiply/subtract. W is ignored and its MAC bits stay untouched.
			constexpr unsigned QUAD_FS_OUTER = 2;  // Q2 -> S8-S11
			constexpr unsigned QUAD_FT_OUTER = 3;  // Q3 -> S12-S15
			constexpr unsigned QUAD_FSYZX = 0;     // Q0 -> S0-S3 (shuffle, then product)
			constexpr unsigned QUAD_FTZXY = 1;     // Q1 -> S4-S7
			const bool opmsub = arithmetic.kind == Cop2MacroArithmeticKind::OpMSub;

			if (!EmitVu0VfAddress(HOST_TMP0, fs) ||
				!m_code.EmitVld1Q32Aligned(QUAD_FS_OUTER, HOST_TMP0) ||
				!EmitVu0VfAddress(HOST_TMP0, ft) ||
				!m_code.EmitVld1Q32Aligned(QUAD_FT_OUTER, HOST_TMP0) ||
				!emit_ensure_norm_consts() ||
				!emit_normalize_quad(QUAD_FS_OUTER) ||
				!emit_normalize_quad(QUAD_FT_OUTER))
			{
				return false;
			}

			// Q0 = {fs.y, fs.z, fs.x, fs.x}, Q1 = {ft.z, ft.x, ft.y, ft.y}.
			if (!m_code.EmitVmovS(0, 9) || !m_code.EmitVmovS(1, 10) ||
				!m_code.EmitVmovS(2, 8) || !m_code.EmitVmovS(3, 8) ||
				!m_code.EmitVmovS(4, 14) || !m_code.EmitVmovS(5, 12) ||
				!m_code.EmitVmovS(6, 13) || !m_code.EmitVmovS(7, 13) ||
				!m_code.EmitVmulF32Q(QUAD_FSYZX, QUAD_FSYZX, QUAD_FTZXY))
			{
				return false;
			}

			if (opmsub)
			{
				// FS is dead once the shuffled product is complete. Reuse Q2 for ACC
				// so the outer path needs no fifth low quad (the old Q4), while still
				// snapshotting ACC before any destination lane is stored.
				if (!EmitVu0RegisterAddress(HOST_TMP0, VU0_ACC_OFFSET) ||
					!m_code.EmitVld1Q32Aligned(QUAD_FS_OUTER, HOST_TMP0) ||
					!emit_normalize_quad(QUAD_FS_OUTER) ||
					!m_code.EmitVsubF32Q(QUAD_FSYZX, QUAD_FS_OUTER, QUAD_FSYZX))
				{
					return false;
				}
			}

			for (unsigned lane = 0; lane < 3; lane++)
			{
				if (!m_code.EmitVmovSToCore(HOST_TMP0, QUAD_FSYZX * 4 + lane) ||
					!emit_update_mac_lane(lane) ||
					!emit_store_result(lane))
	{
					return false;
	}
			}

			return emit_sync_msflags();
		}

		if (!emit_prepare_broadcast_operand())
			return false;

		for (unsigned lane = 0; lane < 4; lane++)
		{
			const unsigned lane_mask = 1u << (3 - lane);
			const bool active_lane = is_outer_product ? (lane < 3) : ((mask & lane_mask) != 0);
			if (!active_lane)
			{
				if (!is_outer_product && !emit_clear_mac_lane(lane))
					return false;
				continue;
			}

			const unsigned source_lane = is_outer_product ? ((lane + 1) % 3) : lane;
			const unsigned operand_lane = is_outer_product ? ((lane + 2) % 3) : lane;
			if (!emit_load_vf_lane(HOST_TMP0, fs, source_lane) ||
				!emit_load_operand_lane_raw(operand_lane) ||
				(use_addi_triace_hack && !emit_apply_triace_add_hack()) ||
				!emit_normalize_vu_float_word(HOST_TMP0) ||
				!emit_normalize_vu_float_word(HOST_TMP1) ||
				!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) ||
				!m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1))
			{
				return false;
			}

			if (uses_acc_source &&
				(!EmitVu0RegisterAddress(HOST_TMP2, VU0_ACC_OFFSET) ||
					!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP2, static_cast<u16>(lane * sizeof(u32))) ||
					!emit_normalize_vu_float_word(HOST_TMP2) ||
					!m_code.EmitVmovCoreToS(VFP_ACC_S4, HOST_TMP2)))
			{
				return false;
			}

			switch (arithmetic.kind)
			{
				case Cop2MacroArithmeticKind::Add:
					if (!m_code.EmitVaddF32(VFP_RESULT_S2, VFP_FS_S0, VFP_FT_S1))
						return false;
					break;
				case Cop2MacroArithmeticKind::Sub:
					if (!m_code.EmitVsubF32(VFP_RESULT_S2, VFP_FS_S0, VFP_FT_S1))
						return false;
					break;
				case Cop2MacroArithmeticKind::Mul:
					if (!m_code.EmitVmulF32(VFP_RESULT_S2, VFP_FS_S0, VFP_FT_S1))
						return false;
					break;
				case Cop2MacroArithmeticKind::OpMula:
					if (!m_code.EmitVmulF32(VFP_RESULT_S2, VFP_FS_S0, VFP_FT_S1))
						return false;
					break;
				case Cop2MacroArithmeticKind::MAdd:
					if (!m_code.EmitVmulF32(VFP_RESULT_S2, VFP_FS_S0, VFP_FT_S1) ||
						!m_code.EmitVaddF32(VFP_RESULT_S2, VFP_ACC_S4, VFP_RESULT_S2))
	{
						return false;
	}
					break;
				case Cop2MacroArithmeticKind::MSub:
					if (!m_code.EmitVmulF32(VFP_RESULT_S2, VFP_FS_S0, VFP_FT_S1) ||
						!m_code.EmitVsubF32(VFP_RESULT_S2, VFP_ACC_S4, VFP_RESULT_S2))
	{
						return false;
	}
					break;
				case Cop2MacroArithmeticKind::OpMSub:
					if (!m_code.EmitVmulF32(VFP_RESULT_S2, VFP_FS_S0, VFP_FT_S1) ||
						!m_code.EmitVsubF32(VFP_RESULT_S2, VFP_ACC_S4, VFP_RESULT_S2))
	{
						return false;
	}
					break;
			}

			if (!m_code.EmitVmovSToCore(HOST_TMP0, VFP_RESULT_S2) ||
				!emit_update_mac_lane(lane) ||
				!emit_store_result(lane))
			{
				return false;
			}
		}

		return emit_sync_msflags();
	}

	bool BlockCompiler::EmitVu0ViBackup(unsigned vi_reg)
	{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		return false;
#else
		// PCSX2 owner: VUops.cpp::_vuBackupVI(). Repeated writes to the same VI
		// register must keep the old value from before the write chain.
		if (!EmitVu0RegisterAddress(HOST_TMP0, VU0_VI_BACKUP_CYCLES_OFFSET) ||
			!EmitVu0RegisterAddress(HOST_TMP2, VU0_VI_REG_NUMBER_OFFSET) ||
			!m_code.EmitLdrbImm12(HOST_TMP1, HOST_TMP0, 0) ||
			!m_code.EmitCmpImm32(HOST_TMP1, 0))
		{
			return false;
		}

		const size_t set_new_from_zero = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (set_new_from_zero == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitLdrImm12(HOST_TMP3, HOST_TMP2, 0) ||
			!m_code.EmitCmpImm32(HOST_TMP3, vi_reg))
		{
			return false;
		}

		const size_t set_new_from_different = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (set_new_from_different == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitMovImm8(HOST_TMP1, 2) ||
			!m_code.EmitStrbImm12(HOST_TMP1, HOST_TMP0, 0))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t set_new_target = m_code.Size();
		if (!m_code.PatchBranch(set_new_from_zero, set_new_target, VitaA32::Condition::EQ) ||
			!m_code.PatchBranch(set_new_from_different, set_new_target, VitaA32::Condition::NE) ||
			!m_code.EmitMovImm8(HOST_TMP1, 2) ||
			!m_code.EmitStrbImm12(HOST_TMP1, HOST_TMP0, 0) ||
			!m_code.EmitMovImm8(HOST_TMP3, static_cast<u8>(vi_reg)) ||
			!m_code.EmitStrImm12(HOST_TMP3, HOST_TMP2, 0) ||
			!EmitVu0ViAddress(HOST_TMP4, vi_reg) ||
			!m_code.EmitLdrhImm8(HOST_TMP3, HOST_TMP4, 0) ||
			!EmitVu0RegisterAddress(HOST_TMP4, VU0_VI_OLD_VALUE_OFFSET) ||
			!m_code.EmitStrImm12(HOST_TMP3, HOST_TMP4, 0))
		{
			return false;
		}

		return m_code.PatchBranch(done, m_code.Size());
#endif
	}

	bool BlockCompiler::EmitCOP2MacroViBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuIADD()/IADDI/IAND/IOR/ISUB. These write
		// only VI.US[0]/SS[0] and update _vuBackupVI() before the halfword store.
		const Cop2MacroViOp vi = DecodeCop2MacroVi(op);
		if (!vi.valid)
			return false;

		const unsigned it = RT(op) & 0x0f;
		const unsigned is = RD(op) & 0x0f;
		const unsigned id = SA(op) & 0x0f;
		const bool immediate = vi.kind == Cop2MacroViKind::AddImmediate;
		const bool logical = vi.kind == Cop2MacroViKind::And || vi.kind == Cop2MacroViKind::Or;
		const unsigned dest = immediate ? it : id;
		if (dest == 0)
			return true;

		if (!EmitVu0ViBackup(dest) ||
			!EmitVu0ViAddress(HOST_TMP0, is) ||
			!(logical ? m_code.EmitLdrhImm8(HOST_TMP2, HOST_TMP0, 0) :
						m_code.EmitLdrshImm8(HOST_TMP2, HOST_TMP0, 0)))
		{
			return false;
		}

		if (immediate)
		{
			const unsigned imm5 = SA(op) & 0x1f;
			const int imm = (imm5 & 0x10) ? static_cast<int>(imm5) - 0x20 : static_cast<int>(imm5);
			if (imm > 0)
			{
				if (!m_code.EmitAddImm8(HOST_TMP2, HOST_TMP2, static_cast<u8>(imm)))
					return false;
			}
			else if (imm < 0 && !m_code.EmitSubImm8(HOST_TMP2, HOST_TMP2, static_cast<u8>(-imm)))
			{
				return false;
			}
		}
		else
		{
			if (!EmitVu0ViAddress(HOST_TMP1, it) ||
				!(logical ? m_code.EmitLdrhImm8(HOST_TMP3, HOST_TMP1, 0) :
							m_code.EmitLdrshImm8(HOST_TMP3, HOST_TMP1, 0)))
			{
				return false;
			}

			switch (vi.kind)
			{
				case Cop2MacroViKind::Add:
					if (!m_code.EmitAddReg(HOST_TMP2, HOST_TMP2, HOST_TMP3))
						return false;
					break;
				case Cop2MacroViKind::Sub:
					if (!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP3))
						return false;
					break;
				case Cop2MacroViKind::And:
					if (!m_code.EmitAndReg(HOST_TMP2, HOST_TMP2, HOST_TMP3))
						return false;
					break;
				case Cop2MacroViKind::Or:
					if (!m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP3))
						return false;
					break;
				case Cop2MacroViKind::AddImmediate:
					return false;
			}
		}

		return EmitVu0ViAddress(HOST_TMP0, dest) &&
			   m_code.EmitStrhImm8(HOST_TMP2, HOST_TMP0, 0);
	}

	bool BlockCompiler::EmitCOP2MacroViTransferBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuMFIR() / _vuMTIR(). MFIR sign-extends
		// VI.SS[0] into selected VF lanes; MTIR stores the selected VF lane's
		// low 16 bits into VI.US[0] after _vuBackupVI().
		const Cop2MacroViTransferOp transfer = DecodeCop2MacroViTransfer(op);
		if (!transfer.valid)
			return false;

		if (transfer.vi_to_vf)
		{
			const unsigned ft = RT(op);
			const unsigned mask = (op >> 21) & 0x0f;
			if (ft == 0 || mask == 0)
				return true;

			const unsigned is = RD(op) & 0x0f;
			if (!EmitVu0ViAddress(HOST_TMP0, is) ||
				!m_code.EmitLdrshImm8(HOST_TMP2, HOST_TMP0, 0))
			{
				return false;
			}

			if (mask == 0x0f)
			{
				constexpr unsigned NEON_VALUE = 0;
				return m_code.EmitVdupI32QFromCore(NEON_VALUE, HOST_TMP2) &&
					   EmitVu0VfAddress(HOST_TMP1, ft) &&
					   m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP1);
			}

			if (!EmitVu0VfAddress(HOST_TMP1, ft))
				return false;

			const auto emit_lane = [&](unsigned lane) {
				const u16 offset = static_cast<u16>(lane * sizeof(u32));
				return m_code.EmitStrImm12(HOST_TMP2, HOST_TMP1, offset);
			};

			if ((mask & 0x8) && !emit_lane(0))
				return false;
			if ((mask & 0x4) && !emit_lane(1))
				return false;
			if ((mask & 0x2) && !emit_lane(2))
				return false;
			if ((mask & 0x1) && !emit_lane(3))
				return false;
			return true;
		}

		const unsigned it = RT(op) & 0x0f;
		if (it == 0)
			return true;

		const unsigned fs = RD(op);
		const unsigned lane = (op >> 21) & 0x03;
		const u8 source_offset = static_cast<u8>(lane * sizeof(u32));
		return EmitVu0ViBackup(it) &&
			   EmitVu0VfAddress(HOST_TMP0, fs) &&
			   m_code.EmitLdrhImm8(HOST_TMP2, HOST_TMP0, source_offset) &&
			   EmitVu0ViAddress(HOST_TMP1, it) &&
			   m_code.EmitStrhImm8(HOST_TMP2, HOST_TMP1, 0);
	}

	bool BlockCompiler::EmitVu0RandomAdvance()
	{
		// PCSX2 owner: VUops.cpp::AdvanceLFSR(). Keep the low-bit tap order and
		// final exponent/mantissa policy exact for REG_R.
		return EmitVu0ViAddress(HOST_TMP0, VU0_REG_R) &&
			   m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSR, 4) &&
			   m_code.EmitEorRegShiftImm(HOST_TMP2, HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSR, 22) &&
			   EmitAndImm32OrReg(HOST_TMP2, HOST_TMP2, 1u, HOST_TMP3) &&
			   m_code.EmitOrrRegShiftImm(HOST_TMP1, HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSL, 1) &&
			   EmitAndImm32OrReg(HOST_TMP1, HOST_TMP1, 0x007fffffu, HOST_TMP3) &&
			   EmitOrrImm32OrReg(HOST_TMP1, HOST_TMP1, 0x3f800000u, HOST_TMP3) &&
			   m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0);
	}

	bool BlockCompiler::EmitCOP2MacroRandomBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuWAITQ() / _vuRINIT() / _vuRGET() /
		// _vuRNEXT() / _vuRXOR(). RNEXT returns before AdvanceLFSR when Ft is
		// zero, but otherwise advances REG_R even when the XYZW mask writes no
		// VF lanes.
		const Cop2MacroRandomOp random = DecodeCop2MacroRandom(op);
		if (!random.valid)
			return false;

		if (random.kind == Cop2MacroRandomKind::WaitQ)
			return true;

		if (random.kind == Cop2MacroRandomKind::RInit ||
			random.kind == Cop2MacroRandomKind::RXor)
		{
			const unsigned fs = RD(op);
			const unsigned lane = (op >> 21) & 0x03;
			const u8 source_offset = static_cast<u8>(lane * sizeof(u32));
			if (!EmitVu0ViAddress(HOST_TMP0, VU0_REG_R) ||
				!EmitVu0VfAddress(HOST_TMP1, fs) ||
				!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP1, source_offset))
			{
				return false;
			}

			if (random.kind == Cop2MacroRandomKind::RXor &&
				(!m_code.EmitLdrImm12(HOST_TMP3, HOST_TMP0, 0) ||
				 !m_code.EmitEorReg(HOST_TMP2, HOST_TMP2, HOST_TMP3)))
			{
				return false;
			}

			return EmitAndImm32OrReg(HOST_TMP2, HOST_TMP2, 0x007fffffu, HOST_TMP3) &&
				   EmitOrrImm32OrReg(HOST_TMP2, HOST_TMP2, 0x3f800000u, HOST_TMP3) &&
				   m_code.EmitStrImm12(HOST_TMP2, HOST_TMP0, 0);
		}

		const unsigned ft = RT(op);
		if (ft == 0)
			return true;

		if (random.kind == Cop2MacroRandomKind::RNext &&
			!EmitVu0RandomAdvance())
		{
			return false;
		}

		const unsigned mask = (op >> 21) & 0x0f;
		if (mask == 0)
			return true;

		if (!EmitVu0ViAddress(HOST_TMP0, VU0_REG_R) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP0, 0))
		{
			return false;
		}

		if (mask == 0x0f)
		{
			constexpr unsigned NEON_VALUE = 0;
			return m_code.EmitVdupI32QFromCore(NEON_VALUE, HOST_TMP2) &&
				   EmitVu0VfAddress(HOST_TMP1, ft) &&
				   m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP1);
		}

		if (!EmitVu0VfAddress(HOST_TMP1, ft))
			return false;

		const auto emit_lane = [this](unsigned lane) {
			const u16 offset = static_cast<u16>(lane * sizeof(u32));
			return m_code.EmitStrImm12(HOST_TMP2, HOST_TMP1, offset);
		};

		if ((mask & 0x8) && !emit_lane(0))
			return false;
		if ((mask & 0x4) && !emit_lane(1))
			return false;
		if ((mask & 0x2) && !emit_lane(2))
			return false;
		if ((mask & 0x1) && !emit_lane(3))
			return false;

		return true;
	}

	bool BlockCompiler::EmitVu0IndexedMemoryAddress(unsigned vi_reg)
	{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		(void)vi_reg;
		return false;
#else
		// PCSX2 owner: VUops.cpp::GET_VU_MEM(). For VU0, addresses below
		// 0x4000 wrap in VU0 data memory; 0x4000 maps into VU1 VF/VI storage.
		if (!EmitVu0ViAddress(HOST_TMP0, vi_reg) ||
			!m_code.EmitLdrhImm8(HOST_TMP2, HOST_TMP0, 0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSL, 4) ||
			!m_code.EmitTstImm32(HOST_TMP2, 0x4000u))
		{
			return false;
		}

		const size_t vu0_memory = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (vu0_memory == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitMovImm32(HOST_TMP0,
				static_cast<u32>(reinterpret_cast<uptr>(&VU1.VF[0]))) ||
			!EmitAndImm32OrReg(HOST_TMP2, HOST_TMP2, 0x03ffu, HOST_TMP3) ||
			!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t vu0_memory_target = m_code.Size();
		if (!m_code.PatchBranch(vu0_memory, vu0_memory_target, VitaA32::Condition::EQ) ||
			!EmitVu0RegisterAddress(HOST_TMP0, VU0_MEM_OFFSET) ||
			!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP0, 0) ||
			!EmitAndImm32OrReg(HOST_TMP2, HOST_TMP2, 0x0fffu, HOST_TMP3) ||
			!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
		{
			return false;
		}

		return m_code.PatchBranch(done, m_code.Size());
#endif
	}

	bool BlockCompiler::EmitCOP2MacroIndexedViMemoryBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuILWR() / _vuISWR(). These use VI.US[0]
		// as an indexed VU memory qword address and do not update _vuBackupVI().
		const Cop2MacroIndexedViMemoryOp memory = DecodeCop2MacroIndexedViMemory(op);
		if (!memory.valid)
			return false;

		const unsigned mask = (op >> 21) & 0x0f;
		if (mask == 0)
			return true;

		const unsigned it = RT(op) & 0x0f;
		const unsigned is = RD(op) & 0x0f;
		if (memory.load)
		{
			if (it == 0)
				return true;

			unsigned lane = 0;
			if (mask & 0x1)
				lane = 3;
			else if (mask & 0x2)
				lane = 2;
			else if (mask & 0x4)
				lane = 1;

			const u8 memory_offset = static_cast<u8>(lane * sizeof(u32));
			return EmitVu0IndexedMemoryAddress(is) &&
				   m_code.EmitLdrhImm8(HOST_TMP2, HOST_TMP0, memory_offset) &&
				   EmitVu0ViAddress(HOST_TMP1, it) &&
				   m_code.EmitStrhImm8(HOST_TMP2, HOST_TMP1, 0);
		}

		if (!EmitVu0IndexedMemoryAddress(is) ||
			!EmitVu0ViAddress(HOST_TMP1, it) ||
			!m_code.EmitLdrhImm8(HOST_TMP2, HOST_TMP1, 0) ||
			!m_code.EmitMovImm8(HOST_TMP3, 0))
		{
			return false;
		}

		const auto emit_lane = [this](unsigned lane) {
			const u8 offset = static_cast<u8>(lane * sizeof(u32));
			return m_code.EmitStrhImm8(HOST_TMP2, HOST_TMP0, offset) &&
				   m_code.EmitStrhImm8(HOST_TMP3, HOST_TMP0, static_cast<u8>(offset + sizeof(u16)));
		};

		if ((mask & 0x8) && !emit_lane(0))
			return false;
		if ((mask & 0x4) && !emit_lane(1))
			return false;
		if ((mask & 0x2) && !emit_lane(2))
			return false;
		if ((mask & 0x1) && !emit_lane(3))
			return false;

		return true;
	}

	bool BlockCompiler::EmitVu0ViLowHalfwordAdjust(unsigned vi_reg, bool decrement)
	{
		return EmitVu0ViAddress(HOST_TMP1, vi_reg) &&
			   m_code.EmitLdrhImm8(HOST_TMP2, HOST_TMP1, 0) &&
			   (decrement ? m_code.EmitSubImm8(HOST_TMP2, HOST_TMP2, 1) :
							m_code.EmitAddImm8(HOST_TMP2, HOST_TMP2, 1)) &&
			   m_code.EmitStrhImm8(HOST_TMP2, HOST_TMP1, 0);
	}

	bool BlockCompiler::EmitCOP2MacroIndexedVectorMemoryBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuLQI() / _vuLQD() / _vuSQI() / _vuSQD().
		// Preserve the exact VI backup and pre/post update order around the VU
		// memory access; running VU0 still exits through the helper interlock.
		const Cop2MacroIndexedVectorMemoryOp memory = DecodeCop2MacroIndexedVectorMemory(op);
		if (!memory.valid)
			return false;

		const bool load = memory.kind == Cop2MacroIndexedVectorMemoryKind::LoadIncrement ||
						  memory.kind == Cop2MacroIndexedVectorMemoryKind::LoadDecrement;
		const bool decrement = memory.kind == Cop2MacroIndexedVectorMemoryKind::LoadDecrement ||
							   memory.kind == Cop2MacroIndexedVectorMemoryKind::StoreDecrement;
		const unsigned ft = RT(op);
		const unsigned fs = RD(op);
		const unsigned vi = load ? (fs & 0x0f) : (ft & 0x0f);
		const unsigned mask = (op >> 21) & 0x0f;

		if (!EmitVu0ViBackup(vi))
			return false;

		if (decrement && (load ? vi != 0 : ft != 0) &&
			!EmitVu0ViLowHalfwordAdjust(vi, true))
		{
			return false;
		}

		const auto emit_post_increment = [&]() {
			const bool do_increment = load ? fs != 0 : ft != 0;
			return !do_increment || EmitVu0ViLowHalfwordAdjust(vi, false);
		};

		if (load)
		{
			if (ft != 0 && mask != 0)
			{
				constexpr unsigned NEON_VALUE = 0;
				if (!EmitVu0IndexedMemoryAddress(vi) ||
					!EmitVu0VfAddress(HOST_TMP1, ft) ||
					!EmitCOP2MacroCopySelectedLanes(mask, HOST_TMP0, HOST_TMP1, NEON_VALUE))
	{
					return false;
	}
			}

			return decrement || emit_post_increment();
		}

		if (mask != 0)
		{
			if (!EmitVu0IndexedMemoryAddress(vi) ||
				!EmitVu0VfAddress(HOST_TMP1, fs))
			{
				return false;
			}

			constexpr unsigned NEON_VALUE = 0;
			if (!EmitCOP2MacroCopySelectedLanes(mask, HOST_TMP1, HOST_TMP0, NEON_VALUE))
			{
				return false;
			}
		}

		return decrement || emit_post_increment();
	}

	// Per-lane bit weights for packing the six VCLIP comparison lanes into the
	// clip flag with one horizontal reduce instead of six vmov lane extractions.
	// Fs.xyz > +|Ft.w| -> bits 0/2/4, Fs.xyz < -|Ft.w| -> bits 1/3/5; lane 3 is
	// unused. The weights are disjoint, so an OR-reduce across the lanes yields
	// the packed value. 16-byte aligned for the aligned vld1.
	alignas(16) static const u32 kClipPosWeights[4] = {1u << 0, 1u << 2, 1u << 4, 0u};
	alignas(16) static const u32 kClipNegWeights[4] = {1u << 1, 1u << 3, 1u << 5, 0u};

	bool BlockCompiler::EmitCOP2MacroClipBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuCLIP() and VCLIPw(). The macro shifts
		// the 24-bit clip history, emits signed raw-bit comparisons for Fs.xyz
		// against abs(Ft.w), mirrors clipflag into VI[REG_CLIP_FLAG], and has
		// no MAC/status/FDIV side effects.
		if (!IsCop2MacroClip(op))
			return false;

		const unsigned ft = RT(op);
		const unsigned fs = RD(op);

		if (!EmitVu0VfAddress(HOST_TMP0, ft) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, static_cast<u16>(3 * sizeof(u32))) ||
			!EmitAndImm32OrReg(HOST_TMP2, HOST_TMP1, FPU_FLOAT_EXPONENT_MASK, HOST_TMP3, true))
		{
			return false;
		}

		const size_t denormal_limit = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (denormal_limit == static_cast<size_t>(-1))
			return false;

		if (!EmitBicImm32OrReg(HOST_TMP2, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP3))
			return false;

		const size_t limit_done = m_code.EmitBranchPlaceholder();
		if (limit_done == static_cast<size_t>(-1))
			return false;

		const size_t denormal_target = m_code.Size();
		if (!m_code.PatchBranch(denormal_limit, denormal_target, VitaA32::Condition::EQ) ||
			!m_code.EmitMovImm32(HOST_TMP2, 0x007fffffu) ||
			!m_code.PatchBranch(limit_done, m_code.Size()))
		{
			return false;
		}

		constexpr unsigned NEON_FS_POS = 0;
		constexpr unsigned NEON_FS_NEG = 1;
		constexpr unsigned NEON_LIMIT = 2;
		constexpr unsigned NEON_SIGN = 3;
		if (!EmitVu0VfAddress(HOST_TMP0, fs) ||
			!m_code.EmitVld1Q32Aligned(NEON_FS_POS, HOST_TMP0) ||
			!m_code.EmitVdupI32QFromCore(NEON_LIMIT, HOST_TMP2) ||
			!m_code.EmitMovImm32(HOST_TMP3, FPU_FLOAT_SIGN_MASK) ||
			!m_code.EmitVdupI32QFromCore(NEON_SIGN, HOST_TMP3) ||
			!m_code.EmitVeorQ(NEON_FS_NEG, NEON_FS_POS, NEON_SIGN) ||
			!m_code.EmitVcgtS32Q(NEON_FS_POS, NEON_FS_POS, NEON_LIMIT) ||
			!m_code.EmitVcgtS32Q(NEON_FS_NEG, NEON_FS_NEG, NEON_LIMIT))
		{
			return false;
		}

		// Pack the six comparison lanes with one horizontal OR-reduce instead of
		// six vmov lane extractions + shifts. Each vcgt lane is 0 or all-ones;
		// ANDing with the disjoint per-lane weights leaves each lane holding its
		// clip bit (or 0), and OR-reducing the four lanes packs bits 0-5 into one
		// word. NEON_LIMIT/NEON_SIGN are dead after the compares, so reuse them.
		constexpr unsigned NEON_PACK_TMP = 4;
		if (!m_code.EmitMovImm32(HOST_TMP0, static_cast<u32>(reinterpret_cast<uptr>(kClipPosWeights))) ||
			!m_code.EmitVld1Q32Aligned(NEON_LIMIT, HOST_TMP0) ||
			!m_code.EmitMovImm32(HOST_TMP0, static_cast<u32>(reinterpret_cast<uptr>(kClipNegWeights))) ||
			!m_code.EmitVld1Q32Aligned(NEON_SIGN, HOST_TMP0) ||
			!m_code.EmitVandQ(NEON_FS_POS, NEON_FS_POS, NEON_LIMIT) ||
			!m_code.EmitVandQ(NEON_FS_NEG, NEON_FS_NEG, NEON_SIGN) ||
			!m_code.EmitVorrQ(NEON_FS_POS, NEON_FS_POS, NEON_FS_NEG) ||
			!m_code.EmitVextI8Q(NEON_PACK_TMP, NEON_FS_POS, NEON_FS_POS, 8) ||
			!m_code.EmitVorrQ(NEON_FS_POS, NEON_FS_POS, NEON_PACK_TMP) ||
			!m_code.EmitVextI8Q(NEON_PACK_TMP, NEON_FS_POS, NEON_FS_POS, 4) ||
			!m_code.EmitVorrQ(NEON_FS_POS, NEON_FS_POS, NEON_PACK_TMP) ||
			!m_code.EmitVmovSToCore(HOST_TMP4, NEON_FS_POS * 4))
		{
			return false;
		}

		return EmitVu0ClipflagAddress(HOST_TMP0) &&
			   m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, 6) &&
			   m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP4) &&
			   EmitAndImm32OrReg(HOST_TMP1, HOST_TMP1, 0x00ffffffu, HOST_TMP2) &&
			   m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0) &&
			   EmitVu0ViAddress(HOST_TMP0, VU0_REG_CLIP_FLAG) &&
			   m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0);
	}

	bool BlockCompiler::EmitCOP2MacroItofBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::intToFloat<>() and _vuITOF*(). VITOF
		// treats source lanes as signed 32-bit integers, converts to float,
		// scales by 2^-offset, and writes only selected destination lanes.
		const Cop2MacroItofOp itof = DecodeCop2MacroItof(op);
		if (!itof.valid)
			return false;

		const unsigned ft = RT(op);
		const unsigned mask = (op >> 21) & 0x0f;
		if (ft == 0 || mask == 0)
			return true;

		constexpr unsigned NEON_VALUE = 0;
		constexpr unsigned NEON_SCALE = 1;
		const unsigned fs = RD(op);
		if (!EmitVu0VfAddress(HOST_TMP0, fs) ||
			!m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP0) ||
			!m_code.EmitVcvtF32S32Q(NEON_VALUE, NEON_VALUE))
		{
			return false;
		}

		if (itof.offset != 0)
		{
			const u32 scale_bits = 0x3f800000u - (itof.offset << 23);
			if (!m_code.EmitMovImm32(HOST_TMP2, scale_bits) ||
				!m_code.EmitVdupI32QFromCore(NEON_SCALE, HOST_TMP2) ||
				!m_code.EmitVmulF32Q(NEON_VALUE, NEON_VALUE, NEON_SCALE))
			{
				return false;
			}
		}

		return EmitCOP2MacroStoreVfSelectedLanes(ft, mask, NEON_VALUE, HOST_TMP1);
	}

	bool BlockCompiler::EmitCOP2MacroFtoiBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::floatToInt<>() and _vuFTOI*(). VFTOI
		// scales source floats by 2^offset, truncates toward zero, and
		// saturates any exponent at or above signed 32-bit range.
		const Cop2MacroFtoiOp ftoi = DecodeCop2MacroFtoi(op);
		if (!ftoi.valid)
			return false;

		const unsigned ft = RT(op);
		const unsigned mask = (op >> 21) & 0x0f;
		if (ft == 0 || mask == 0)
			return true;

		constexpr unsigned NEON_VALUE = 0;
		constexpr unsigned NEON_BITS = 1;
		constexpr unsigned NEON_MASK = 2;
		constexpr unsigned NEON_SATURATED = 3;
		const unsigned fs = RD(op);
		if (!EmitVu0VfAddress(HOST_TMP0, fs) ||
			!m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP0))
		{
			return false;
		}

		if (ftoi.offset != 0)
		{
			const u32 scale_bits = 0x3f800000u + (ftoi.offset << 23);
			if (!m_code.EmitMovImm32(HOST_TMP2, scale_bits) ||
				!m_code.EmitVdupI32QFromCore(NEON_BITS, HOST_TMP2) ||
				!m_code.EmitVmulF32Q(NEON_VALUE, NEON_VALUE, NEON_BITS))
			{
				return false;
			}
		}

		if (!m_code.EmitVorrQ(NEON_BITS, NEON_VALUE, NEON_VALUE) ||
			!m_code.EmitVcvtS32F32Q(NEON_VALUE, NEON_VALUE))
		{
			return false;
		}

		if (!m_code.EmitMovImm32(HOST_TMP2, 0x7f800000u) ||
			!m_code.EmitVdupI32QFromCore(NEON_MASK, HOST_TMP2) ||
			!m_code.EmitVandQ(NEON_MASK, NEON_BITS, NEON_MASK) ||
			!m_code.EmitMovImm32(HOST_TMP2, 0x4effffffu) ||
			!m_code.EmitVdupI32QFromCore(NEON_SATURATED, HOST_TMP2) ||
			!m_code.EmitVcgtS32Q(NEON_MASK, NEON_MASK, NEON_SATURATED) ||
			!m_code.EmitVshrS32Q(NEON_SATURATED, NEON_BITS, 31) ||
			!m_code.EmitMovImm32(HOST_TMP2, 0x7fffffffu) ||
			!m_code.EmitVdupI32QFromCore(NEON_BITS, HOST_TMP2) ||
			!m_code.EmitVeorQ(NEON_SATURATED, NEON_SATURATED, NEON_BITS) ||
			!m_code.EmitVandQ(NEON_SATURATED, NEON_SATURATED, NEON_MASK) ||
			!m_code.EmitVmvnQ(NEON_MASK, NEON_MASK) ||
			!m_code.EmitVandQ(NEON_VALUE, NEON_VALUE, NEON_MASK) ||
			!m_code.EmitVorrQ(NEON_VALUE, NEON_VALUE, NEON_SATURATED))
		{
			return false;
		}

		return EmitCOP2MacroStoreVfSelectedLanes(ft, mask, NEON_VALUE, HOST_TMP1);
	}

	bool BlockCompiler::EmitCOP2MacroFdivBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuDIV() / _vuSQRT() / _vuRSQRT() and
		// SYNCFDIV(). Macro COP2 updates VU0.q/statusflag immediately and
		// mirrors Q plus D/I status bits into VI[REG_Q]/VI[REG_STATUS_FLAG].
		const Cop2MacroFdivOp fdiv = DecodeCop2MacroFdiv(op);
		if (!fdiv.valid)
			return false;

#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		const bool vu0_overflow_clamp = true;
#else
		const bool vu0_overflow_clamp = CHECK_VU_OVERFLOW(0);
#endif
		const unsigned fs = RD(op);
		const unsigned ft = RT(op);
		const unsigned fsf = (op >> 21) & 0x03;
		const unsigned ftf = (op >> 23) & 0x03;
		constexpr unsigned VFP_FS_S0 = 0;
		constexpr unsigned VFP_FT_S1 = 1;
		constexpr unsigned VFP_Q_S2 = 2;

		const auto emit_load_vf_lane = [&](unsigned host_reg, unsigned vf_reg, unsigned lane) {
			return EmitVu0VfAddress(host_reg, vf_reg) &&
				   m_code.EmitLdrImm12(host_reg, host_reg, static_cast<u16>(lane * sizeof(u32)));
		};

		const auto emit_normalize_vu_float_word = [&](unsigned reg) {
			if (!EmitAndImm32OrReg(HOST_TMP3, reg, FPU_FLOAT_EXPONENT_MASK, HOST_TMP5) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t exponent_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (exponent_nonzero == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP5))
				return false;

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t exponent_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(exponent_nonzero, exponent_nonzero_target, VitaA32::Condition::NE))
				return false;

			size_t done_from_finite = static_cast<size_t>(-1);
			if (vu0_overflow_clamp)
			{
				if (!EmitCmpImm32OrReg(HOST_TMP3, FPU_FLOAT_EXPONENT_MASK, HOST_TMP5))
					return false;

				done_from_finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
				if (done_from_finite == static_cast<size_t>(-1))
					return false;

				if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP5) ||
					!EmitOrrImm32OrReg(reg, reg, FPU_FLOAT_MAX_FINITE, HOST_TMP5))
	{
					return false;
	}
			}

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_from_zero, done_target) &&
				   (done_from_finite == static_cast<size_t>(-1) ||
					   m_code.PatchBranch(done_from_finite, done_target, VitaA32::Condition::NE));
		};

		const auto emit_abs_word = [&](unsigned rd, unsigned rn) {
			return EmitBicImm32OrReg(rd, rn, FPU_FLOAT_SIGN_MASK, HOST_TMP5);
		};

		const auto emit_set_invalid_if_negative_nonzero = [&](unsigned reg) {
			if (!emit_abs_word(HOST_TMP3, reg) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t zero = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (zero == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitTstImm32(reg, FPU_FLOAT_SIGN_MASK))
				return false;

			const size_t positive = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (positive == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm8(HOST_TMP4, 0x10))
				return false;

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(zero, done_target, VitaA32::Condition::EQ) &&
				   m_code.PatchBranch(positive, done_target, VitaA32::Condition::EQ);
		};

		const auto emit_compute_div = [&]() {
			return m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) &&
				   m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1) &&
				   m_code.EmitVdivF32(VFP_Q_S2, VFP_FS_S0, VFP_FT_S1) &&
				   m_code.EmitVmovSToCore(HOST_TMP0, VFP_Q_S2) &&
				   emit_normalize_vu_float_word(HOST_TMP0);
		};

		const auto emit_compute_sqrt_abs_ft = [&]() {
			return emit_abs_word(HOST_TMP1, HOST_TMP1) &&
				   m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1) &&
				   m_code.EmitVsqrtF32(VFP_Q_S2, VFP_FT_S1) &&
				   m_code.EmitVmovSToCore(HOST_TMP0, VFP_Q_S2) &&
				   emit_normalize_vu_float_word(HOST_TMP0);
		};

		const auto emit_store_q_and_status = [&]() {
			if (!EmitVu0RegisterAddress(HOST_TMP1, VU0_Q_OFFSET) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_TMP1, 0) ||
				!EmitVu0ViAddress(HOST_TMP1, VU0_REG_Q) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_TMP1, 0) ||
				!EmitVu0RegisterAddress(HOST_TMP1, VU0_STATUSFLAG_OFFSET) ||
				!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP1, 0) ||
				!EmitBicImm32OrReg(HOST_TMP2, HOST_TMP2, 0x30u, HOST_TMP5) ||
				!m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_TMP1, 0) ||
				!EmitVu0ViAddress(HOST_TMP1, VU0_REG_STATUS_FLAG) ||
				!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP1, 0) ||
				!EmitBicImm32OrReg(HOST_TMP2, HOST_TMP2, 0x0c30u, HOST_TMP5) ||
				!m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
				!m_code.EmitOrrRegShiftImm(HOST_TMP2, HOST_TMP2, HOST_TMP4, VitaA32::ShiftType::LSL, 6) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_TMP1, 0))
			{
				return false;
			}
			return true;
		};

		if (!m_code.EmitMovImm8(HOST_TMP4, 0))
			return false;

		switch (fdiv.kind)
		{
			case Cop2MacroFdivKind::Div:
			{
				if (!emit_load_vf_lane(HOST_TMP0, fs, fsf) ||
					!emit_normalize_vu_float_word(HOST_TMP0) ||
					!emit_load_vf_lane(HOST_TMP1, ft, ftf) ||
					!emit_normalize_vu_float_word(HOST_TMP1) ||
					!emit_abs_word(HOST_TMP3, HOST_TMP1) ||
					!m_code.EmitCmpImm32(HOST_TMP3, 0))
	{
					return false;
	}

				const size_t divisor_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
				if (divisor_nonzero == static_cast<size_t>(-1))
					return false;

				if (!emit_abs_word(HOST_TMP3, HOST_TMP0) ||
					!m_code.EmitMovImm8(HOST_TMP4, 0x20) ||
					!m_code.EmitCmpImm32(HOST_TMP3, 0) ||
					!m_code.EmitMovImm8(HOST_TMP4, 0x10, VitaA32::Condition::EQ) ||
					!m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
					!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP5) ||
					!EmitOrrImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_MAX_FINITE, HOST_TMP5))
	{
					return false;
	}

				const size_t done_from_zero = m_code.EmitBranchPlaceholder();
				if (done_from_zero == static_cast<size_t>(-1))
					return false;

				const size_t divisor_nonzero_target = m_code.Size();
				if (!m_code.PatchBranch(divisor_nonzero, divisor_nonzero_target, VitaA32::Condition::NE) ||
					!emit_compute_div())
	{
					return false;
	}

				return m_code.PatchBranch(done_from_zero, m_code.Size()) &&
					   emit_store_q_and_status();
			}

			case Cop2MacroFdivKind::Sqrt:
				if (!emit_load_vf_lane(HOST_TMP1, ft, ftf) ||
					!emit_normalize_vu_float_word(HOST_TMP1) ||
					!emit_set_invalid_if_negative_nonzero(HOST_TMP1) ||
					!emit_compute_sqrt_abs_ft())
	{
					return false;
	}
				return emit_store_q_and_status();

			case Cop2MacroFdivKind::Rsqrt:
			{
				if (!emit_load_vf_lane(HOST_TMP0, fs, fsf) ||
					!emit_normalize_vu_float_word(HOST_TMP0) ||
					!emit_load_vf_lane(HOST_TMP1, ft, ftf) ||
					!emit_normalize_vu_float_word(HOST_TMP1) ||
					!emit_abs_word(HOST_TMP3, HOST_TMP1) ||
					!m_code.EmitCmpImm32(HOST_TMP3, 0))
	{
					return false;
	}

				const size_t ft_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
				if (ft_nonzero == static_cast<size_t>(-1))
					return false;

				if (!emit_abs_word(HOST_TMP3, HOST_TMP0) ||
					!m_code.EmitMovImm8(HOST_TMP4, 0x20) ||
					!m_code.EmitCmpImm32(HOST_TMP3, 0))
	{
					return false;
	}

				const size_t fs_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
				if (fs_nonzero == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitMovImm8(HOST_TMP4, 0x30) ||
					!m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
					!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP5))
	{
					return false;
	}

				const size_t done_from_zero_zero = m_code.EmitBranchPlaceholder();
				if (done_from_zero_zero == static_cast<size_t>(-1))
					return false;

				const size_t fs_nonzero_target = m_code.Size();
				if (!m_code.PatchBranch(fs_nonzero, fs_nonzero_target, VitaA32::Condition::NE) ||
					!m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
					!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP5) ||
					!EmitOrrImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_MAX_FINITE, HOST_TMP5))
	{
					return false;
	}

				const size_t done_from_zero = m_code.EmitBranchPlaceholder();
				if (done_from_zero == static_cast<size_t>(-1))
					return false;

				const size_t ft_nonzero_target = m_code.Size();
				if (!m_code.PatchBranch(ft_nonzero, ft_nonzero_target, VitaA32::Condition::NE) ||
					!emit_set_invalid_if_negative_nonzero(HOST_TMP1) ||
					!emit_abs_word(HOST_TMP1, HOST_TMP1) ||
					!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) ||
					!m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1) ||
					!m_code.EmitVsqrtF32(VFP_FT_S1, VFP_FT_S1) ||
					!m_code.EmitVdivF32(VFP_Q_S2, VFP_FS_S0, VFP_FT_S1) ||
					!m_code.EmitVmovSToCore(HOST_TMP0, VFP_Q_S2) ||
					!emit_normalize_vu_float_word(HOST_TMP0))
	{
					return false;
	}

				const size_t done_target = m_code.Size();
				return m_code.PatchBranch(done_from_zero_zero, done_target) &&
					   m_code.PatchBranch(done_from_zero, done_target) &&
					   emit_store_q_and_status();
			}
		}

		return false;
	}

	bool BlockCompiler::EmitCOP2MacroMoveBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuMOVE() / _vuMR32(). These are raw
		// bit-copy lane operations; VMR32 keeps self-overlap behavior by reading
		// the original source qword before selected destination lane stores.
		const Cop2MacroMoveOp move = DecodeCop2MacroMove(op);
		if (!move.valid)
			return false;

		const unsigned ft = RT(op);
		const unsigned mask = (op >> 21) & 0x0f;
		if (ft == 0 || mask == 0)
			return true;

		const unsigned fs = RD(op);
		constexpr unsigned NEON_VALUE = 0;
		if (!move.rotate32)
		{
			return EmitVu0VfAddress(HOST_TMP0, fs) &&
				   EmitVu0VfAddress(HOST_TMP1, ft) &&
				   EmitCOP2MacroCopySelectedLanes(mask, HOST_TMP0, HOST_TMP1, NEON_VALUE);
		}

		return EmitVu0VfAddress(HOST_TMP0, fs) &&
			   m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP0) &&
			   m_code.EmitVextI8Q(NEON_VALUE, NEON_VALUE, NEON_VALUE, 4) &&
			   EmitCOP2MacroStoreVfSelectedLanes(ft, mask, NEON_VALUE, HOST_TMP1);
	}

	bool BlockCompiler::EmitCOP2MacroMinMaxBody(u32 op)
	{
		// PCSX2 owner: VUops.cpp::fp_max()/fp_min(). Those helpers compare
		// raw float bits as signed words, but swap min/max selection when both
		// inputs are negative. Keep that bit policy exactly; no host FP here.
		const Cop2MacroMinMaxOp minmax = DecodeCop2MacroMinMax(op);
		if (!minmax.valid)
			return false;

		const unsigned fd = SA(op);
		const unsigned mask = (op >> 21) & 0x0f;
		if (fd == 0 || mask == 0)
			return true;

		constexpr unsigned NEON_FS = 0;
		constexpr unsigned NEON_OPERAND = 1;
		constexpr unsigned NEON_SIGNED_MIN = 2;
		constexpr unsigned NEON_SIGNED_MAX = 3;
		constexpr unsigned NEON_BOTH_NEGATIVE = 4;
		if (!EmitVu0VfAddress(HOST_TMP0, RD(op)) ||
			!m_code.EmitVld1Q32Aligned(NEON_FS, HOST_TMP0))
		{
			return false;
		}

		if (minmax.immediate_operand)
		{
			if (!EmitVu0ViAddress(HOST_TMP5, VU0_REG_I) ||
				!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP5, 0) ||
				!m_code.EmitVdupI32QFromCore(NEON_OPERAND, HOST_TMP2))
			{
				return false;
			}
		}
		else if (minmax.vector_operand)
		{
			if (!EmitVu0VfAddress(HOST_TMP5, RT(op)) ||
				!m_code.EmitVld1Q32Aligned(NEON_OPERAND, HOST_TMP5))
			{
				return false;
			}
		}
		else
		{
			if (!EmitVu0VfAddress(HOST_TMP5, RT(op)) ||
				!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP5,
					static_cast<u16>(minmax.broadcast_lane * sizeof(u32))) ||
				!m_code.EmitVdupI32QFromCore(NEON_OPERAND, HOST_TMP2))
			{
				return false;
			}
		}

		const unsigned negative_result = minmax.take_max ? NEON_SIGNED_MIN : NEON_SIGNED_MAX;
		const unsigned default_result = minmax.take_max ? NEON_SIGNED_MAX : NEON_SIGNED_MIN;
		return m_code.EmitVminS32Q(NEON_SIGNED_MIN, NEON_FS, NEON_OPERAND) &&
			   m_code.EmitVmaxS32Q(NEON_SIGNED_MAX, NEON_FS, NEON_OPERAND) &&
			   m_code.EmitVandQ(NEON_BOTH_NEGATIVE, NEON_FS, NEON_OPERAND) &&
			   m_code.EmitVshrS32Q(NEON_BOTH_NEGATIVE, NEON_BOTH_NEGATIVE, 31) &&
			   m_code.EmitVandQ(NEON_FS, negative_result, NEON_BOTH_NEGATIVE) &&
			   m_code.EmitVmvnQ(NEON_BOTH_NEGATIVE, NEON_BOTH_NEGATIVE) &&
			   m_code.EmitVandQ(NEON_OPERAND, default_result, NEON_BOTH_NEGATIVE) &&
			   m_code.EmitVorrQ(NEON_FS, NEON_FS, NEON_OPERAND) &&
			   EmitCOP2MacroStoreVfSelectedLanes(fd, mask, NEON_FS, HOST_TMP1);
	}

	bool BlockCompiler::EmitCOP2MacroFast(u32 op, u32 next_pc,
		u32 raw_cycles_through_instruction, const void* event_exit)
	{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		return false;
#else
		if (!event_exit || raw_cycles_through_instruction == 0)
			return false;

		size_t vu0_idle = static_cast<size_t>(-1);
		if (!EmitCOP2IdleBranch(&vu0_idle) ||
			!EmitSystemHelperEventExit(op, next_pc, raw_cycles_through_instruction,
				reinterpret_cast<const void*>(&R5900::Interpreter::OpcodeImpl::COP2), event_exit) ||
			!m_code.PatchBranch(vu0_idle, m_code.Size(), VitaA32::Condition::EQ))
		{
			return false;
		}

		return EmitCOP2MacroBody(op);
#endif
	}

	bool BlockCompiler::EmitCOP2InterlockCall(u32 op, bool wait_for_mbit)
	{
		// PCSX2 owners: VU0.cpp interlocked QMFC2/QMTC2/CFC2/CTC2 plus
		// x86/microVU_Macro.inl::COP2_Interlock(). The caller already selected
		// the running-VU0 tail, so bit 0 performs the E-bit finish or M/E-bit wait.
		if ((op & 1u) == 0)
			return true;

		void (*helper)() = wait_for_mbit ? &_vu0WaitMicro : &_vu0FinishMicro;
		return m_code.EmitCallAbsolute(reinterpret_cast<const void*>(helper));
	}

	bool BlockCompiler::EmitCOP2VectorTransferFast(u32 op, u32 next_pc,
		u32 raw_cycles_through_instruction, const void* event_exit)
	{
		// PCSX2 owners: VU0.cpp::QMFC2() / QMTC2() and
		// x86/microVU_Macro.inl::recQMFC2()/recQMTC2(). Idle VU0 transfers stay
		// in the A32 block; running VU0 takes the guarded sync/interlock/event exit.
		if (!event_exit || raw_cycles_through_instruction == 0)
			return false;

		size_t vu0_idle = static_cast<size_t>(-1);
		const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
		const bool wait_for_mbit = ((op >> 21) & 0x1f) == 0x05;
		if (!EmitCOP2IdleBranch(&vu0_idle) ||
			!m_code.EmitMovImm32(HOST_TMP0, op) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
			!EmitStorePc(next_pc) ||
			!EmitAddScaledCyclesToCpu(cycles) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu0Sync)) ||
			!EmitCOP2InterlockCall(op, wait_for_mbit) ||
			!EmitCOP2VectorTransferBody(op) ||
			!EmitEventExitReturn(event_exit) ||
			!m_code.PatchBranch(vu0_idle, m_code.Size(), VitaA32::Condition::EQ))
		{
			return false;
		}

		return EmitCOP2VectorTransferBody(op);
	}

	bool BlockCompiler::EmitCOP2ControlReadFast(u32 op, u32 next_pc,
		u32 raw_cycles_through_instruction, const void* event_exit)
	{
		if (!event_exit || raw_cycles_through_instruction == 0)
			return false;

		size_t vu0_idle = static_cast<size_t>(-1);
		const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
		if (!EmitCOP2IdleBranch(&vu0_idle) ||
			!m_code.EmitMovImm32(HOST_TMP0, op) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
			!EmitStorePc(next_pc) ||
			!EmitAddScaledCyclesToCpu(cycles) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu0Sync)) ||
			!EmitCOP2InterlockCall(op, false) ||
			!EmitCOP2ControlReadBody(op) ||
			!EmitEventExitReturn(event_exit) ||
			!m_code.PatchBranch(vu0_idle, m_code.Size(), VitaA32::Condition::EQ))
		{
			return false;
		}

		return EmitCOP2ControlReadBody(op);
	}

	bool BlockCompiler::EmitCOP2ControlWriteFast(u32 op, u32 next_pc,
		u32 raw_cycles_through_instruction, const void* event_exit)
	{
		if (!event_exit || raw_cycles_through_instruction == 0)
			return false;

		if (RD(op) == VU0_REG_CMSAR1)
		{
			// PCSX2 owners: VU0.cpp::CTC2(REG_CMSAR1) and
			// x86/microVU_Macro.inl::recCTC2(). CMSAR1 starts VU1 at the
			// committed EE cycle, so emit a native event tail instead of a
			// fall-through body under this compiler's deferred-cycle model.
			const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
			if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
				!EmitStorePc(next_pc) ||
				!EmitAddScaledCyclesToCpu(cycles) ||
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu0Sync)))
			{
				return false;
			}

			if ((op & 1u) != 0)
			{
				size_t vu0_idle = static_cast<size_t>(-1);
				if (!EmitVu0ViAddress(HOST_TMP0, VU0_REG_VPU_STAT) ||
					!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0) ||
					!m_code.EmitTstImm32(HOST_TMP1, 1))
	{
					return false;
	}

				vu0_idle = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
				if (vu0_idle == static_cast<size_t>(-1) ||
					!EmitCOP2InterlockCall(op, true) ||
					!m_code.PatchBranch(vu0_idle, m_code.Size(), VitaA32::Condition::EQ))
	{
					return false;
	}
			}

			if (!m_code.EmitMovImm8(HOST_TMP0, 1) ||
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu1Finish)) ||
				!EmitLoadGprLowRawZero(RT(op), HOST_TMP0) ||
				!m_code.EmitUbfx(HOST_TMP0, HOST_TMP0, 0, 16) ||
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu1ExecMicro)) ||
				!EmitEventExitReturn(event_exit))
			{
				return false;
			}

			return true;
		}

		size_t vu0_idle = static_cast<size_t>(-1);
		const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
		if (!EmitCOP2IdleBranch(&vu0_idle) ||
			!m_code.EmitMovImm32(HOST_TMP0, op) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
			!EmitStorePc(next_pc) ||
			!EmitAddScaledCyclesToCpu(cycles) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu0Sync)) ||
			!EmitCOP2InterlockCall(op, true) ||
			!EmitCOP2ControlWriteBody(op) ||
			!EmitEventExitReturn(event_exit) ||
			!m_code.PatchBranch(vu0_idle, m_code.Size(), VitaA32::Condition::EQ))
		{
			return false;
		}

		return EmitCOP2ControlWriteBody(op);
	}

	bool BlockCompiler::EmitCOP1MoveControlFast(u32 op)
	{
		// PCSX2 owners: FPU.cpp::MFC1()/MTC1()/CFC1()/CTC1() and
		// x86/iFPU.cpp::recMFC1()/recMTC1()/recCFC1()/recCTC1(). The ARMv7
		// validation oracle currently runs PCSX2's interpreter-owned FPU.cpp
		// path, so CFC1 keeps that helper's raw fs=31 and fs=0 behavior here.
		const unsigned rt = RT(op);
		const unsigned fs = RD(op);
		const auto load_rt_low = [this, rt](unsigned host_reg) {
			u32 value = 0;
			const bool value_known = TryGetKnownGprLow(rt, &value);
			if (rt != 0 && FindGprPinHost(rt) < 0 && value_known)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuCop1KnownSourceFastPaths++;
#endif
				return m_code.EmitMovImm32(host_reg, value);
			}

			return EmitLoadGprLow(rt, host_reg);
		};

		switch ((op >> 21) & 0x1f)
		{
			case 0x00: // MFC1
				if (rt == 0)
					return true;
	{
					const unsigned result_reg = SelectGprLowResultHost(rt, HOST_TMP0);
					return m_code.EmitLdrImm12(result_reg, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) &&
						   EmitStoreGprSignExtended32FromLow(rt, result_reg);
	}
			case 0x02: // CFC1
				if (rt == 0)
					return true;
	{
					const unsigned result_reg = SelectGprLowResultHost(rt, HOST_TMP0);
					if (fs == 31)
	{
						if (!m_code.EmitLdrImm12(result_reg, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))))
							return false;
	}
					else if (fs == 0)
	{
						if (!m_code.EmitMovImm32(result_reg, 0x00002e00u))
							return false;
	}
					else if (!m_code.EmitMovImm8(result_reg, 0))
	{
						return false;
	}

					return EmitStoreGprSignExtended32FromLow(rt, result_reg);
	}
			case 0x04: // MTC1
				return load_rt_low(HOST_TMP0) &&
					   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs)));
			case 0x06: // CTC1
				if (fs != 31)
					return true;
				return load_rt_low(HOST_TMP0) &&
					   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31)));
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitCOP1ArithmeticFast(u32 op)
	{
		// PCSX2 owners: FPU.cpp::ADD_S()/SUB_S()/MUL_S(), plus
		// FPU.cpp::fpuDouble(), checkOverflow(), and checkUnderflow(). The
		// native path preserves signed zero when normalizing exponent-0 inputs,
		// clamps exponent-0xff inputs to signed max finite, then applies the
		// same O/U cause and SO/SU sticky flag behavior after the VFP operation.
		const unsigned fs = (op >> 11) & 0x1f;
		const unsigned ft = (op >> 16) & 0x1f;
		const unsigned fd = (op >> 6) & 0x1f;
		const u32 function = op & 0x3f;
		constexpr unsigned VFP_FS_S0 = 0;
		constexpr unsigned VFP_FT_S1 = 1;
		constexpr unsigned VFP_FD_S2 = 2;
		const bool same_source = fs == ft;
		const unsigned vfp_ft_source = same_source ? VFP_FS_S0 : VFP_FT_S1;

		const auto normalize_arithmetic_word = [&](unsigned reg) {
			if (!m_code.EmitAndReg(HOST_TMP3, reg, HOST_TMP5) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP5))
			{
				return false;
			}

			const size_t not_infinity_or_nan = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (not_infinity_or_nan == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP4, HOST_TMP5, 1) ||
				!m_code.EmitOrrReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_clamp = m_code.EmitBranchPlaceholder();
			if (done_from_clamp == static_cast<size_t>(-1))
				return false;

			const size_t finite_or_zero_target = m_code.Size();
			if (!m_code.PatchBranch(not_infinity_or_nan, finite_or_zero_target, VitaA32::Condition::NE) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t done_from_finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (done_from_finite == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP4))
			{
				return false;
			}

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_from_clamp, done_target) &&
				   m_code.PatchBranch(done_from_finite, done_target, VitaA32::Condition::NE);
		};
		const auto normalize_tracked_arithmetic_word = [&](unsigned reg, bool already_normalized) {
			if (already_normalized)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuCop1NormalizedOperandSkips++;
#endif
				return true;
			}
			return normalize_arithmetic_word(reg);
		};

		const auto apply_overflow_underflow_flags = [&]() {
			if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) ||
				!m_code.EmitBicImm32(HOST_TMP2, HOST_TMP0, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP5))
			{
				return false;
			}

			const size_t no_overflow = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_overflow == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP2) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP5, 1) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!EmitOrrImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_ARITHMETIC_OVERFLOW_FLAGS, HOST_TMP2))
			{
				return false;
			}

			const size_t store_result = m_code.EmitBranchPlaceholder();
			if (store_result == static_cast<size_t>(-1))
				return false;

			const size_t no_overflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_overflow, no_overflow_target, VitaA32::Condition::NE) ||
				!EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_OVERFLOW_FLAG, HOST_TMP2) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP5) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t no_underflow_from_exponent = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_underflow_from_exponent == static_cast<size_t>(-1))
				return false;

			if (!EmitAndCop1FractionMask(HOST_TMP3, HOST_TMP0) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t no_underflow_from_fraction = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (no_underflow_from_fraction == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP2) ||
				!EmitOrrImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_ARITHMETIC_UNDERFLOW_FLAGS, HOST_TMP2))
			{
				return false;
			}

			const size_t underflow_store = m_code.EmitBranchPlaceholder();
			if (underflow_store == static_cast<size_t>(-1))
				return false;

			const size_t clear_underflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_underflow_from_exponent, clear_underflow_target, VitaA32::Condition::NE) ||
				!m_code.PatchBranch(no_underflow_from_fraction, clear_underflow_target, VitaA32::Condition::EQ) ||
				!EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_UNDERFLOW_FLAG, HOST_TMP2))
			{
				return false;
			}

			const size_t store_target = m_code.Size();
			return m_code.PatchBranch(store_result, store_target) &&
				   m_code.PatchBranch(underflow_store, store_target) &&
				   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd)));
		};

		const auto store_positive_zero_and_clear_overflow_underflow = [&]() {
			return m_code.EmitMovImm8(HOST_TMP0, 0) &&
				   m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_OVERFLOW_UNDERFLOW_FLAGS, HOST_TMP2) &&
				   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd)));
		};

		if (function == 0x01 && fs == ft) // SUB_S
		{
			// PCSX2 FPU.cpp::fpuDouble() maps one source bit-pattern identically
			// on both sides, so SUB_S of the same FPR is +0 and clears O/U causes.
			return store_positive_zero_and_clear_overflow_underflow();
		}

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
			!EmitCop1ExponentMask(HOST_TMP5) ||
			!normalize_tracked_arithmetic_word(HOST_TMP0, IsCop1FprNormalized(fs)) ||
			!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0))
		{
			return false;
		}

		if (!same_source &&
			(!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
				!normalize_tracked_arithmetic_word(HOST_TMP1, IsCop1FprNormalized(ft)) ||
				!m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1)))
		{
			return false;
		}

		switch (function)
		{
			case 0x00: // ADD_S
				if (!m_code.EmitVaddF32(VFP_FD_S2, VFP_FS_S0, vfp_ft_source))
					return false;
				break;
			case 0x01: // SUB_S
				if (!m_code.EmitVsubF32(VFP_FD_S2, VFP_FS_S0, vfp_ft_source))
					return false;
				break;
			case 0x02: // MUL_S
				if (!m_code.EmitVmulF32(VFP_FD_S2, VFP_FS_S0, vfp_ft_source))
					return false;
				break;
			default:
				return false;
		}

		return m_code.EmitVmovSToCore(HOST_TMP0, VFP_FD_S2) &&
			   apply_overflow_underflow_flags();
	}

	bool BlockCompiler::EmitCOP1DivSqrtFast(u32 op)
	{
		// PCSX2 owners: FPU.cpp::DIV_S()/SQRT_S()/RSQRT_S(), plus
		// FPU.cpp::checkDivideByZero(), fpuDouble(), checkOverflow(), and
		// checkUnderflow(). DIV/RSQRT clamp overflow/underflow results without
		// touching O/U flags because FPU.cpp passes cFlagsToSet=0. SQRT clears
		// only I/D cause flags and does not run overflow/underflow checks.
		const unsigned fs = (op >> 11) & 0x1f;
		const unsigned ft = (op >> 16) & 0x1f;
		const unsigned fd = (op >> 6) & 0x1f;
		const u32 function = op & 0x3f;
		constexpr unsigned VFP_FS_S0 = 0;
		constexpr unsigned VFP_FT_S1 = 1;
		constexpr unsigned VFP_FD_S2 = 2;
		const bool same_source = fs == ft;

		const auto normalize_arithmetic_word_with_mask = [&](unsigned reg, unsigned exponent_mask_reg) {
			if (!m_code.EmitAndReg(HOST_TMP3, reg, exponent_mask_reg) ||
				!m_code.EmitCmpReg(HOST_TMP3, exponent_mask_reg))
			{
				return false;
			}

			const size_t not_infinity_or_nan = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (not_infinity_or_nan == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP4, exponent_mask_reg, 1) ||
				!m_code.EmitOrrReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_clamp = m_code.EmitBranchPlaceholder();
			if (done_from_clamp == static_cast<size_t>(-1))
				return false;

			const size_t finite_or_zero_target = m_code.Size();
			if (!m_code.PatchBranch(not_infinity_or_nan, finite_or_zero_target, VitaA32::Condition::NE) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t done_from_finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (done_from_finite == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP4))
			{
				return false;
			}

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_from_clamp, done_target) &&
				   m_code.PatchBranch(done_from_finite, done_target, VitaA32::Condition::NE);
		};
		const auto normalize_tracked_arithmetic_word_with_mask =
			[&](unsigned reg, unsigned exponent_mask_reg, bool already_normalized) {
				if (already_normalized)
	{
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuCop1NormalizedOperandSkips++;
#endif
					return true;
	}
				return normalize_arithmetic_word_with_mask(reg, exponent_mask_reg);
			};

		const auto store_result = [&](bool store_fcr31) {
			if (store_fcr31 &&
				!m_code.EmitStrImm12(HOST_TMP5, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))))
			{
				return false;
			}
			return m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd)));
		};

		const auto clamp_result_no_flags_with_mask = [&](bool store_fcr31, unsigned exponent_mask_reg) {
			if (!m_code.EmitBicImm32(HOST_TMP2, HOST_TMP0, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitCmpReg(HOST_TMP2, exponent_mask_reg))
			{
				return false;
			}

			const size_t no_overflow = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_overflow == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP2) ||
				!m_code.EmitSubImm8(HOST_TMP2, exponent_mask_reg, 1) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
			{
				return false;
			}

			const size_t store_after_overflow = m_code.EmitBranchPlaceholder();
			if (store_after_overflow == static_cast<size_t>(-1))
				return false;

			const size_t no_overflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_overflow, no_overflow_target, VitaA32::Condition::NE) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, exponent_mask_reg) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t no_underflow_from_exponent = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_underflow_from_exponent == static_cast<size_t>(-1))
				return false;

			if (!EmitAndCop1FractionMask(HOST_TMP3, HOST_TMP0) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t no_underflow_from_fraction = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (no_underflow_from_fraction == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP2))
			{
				return false;
			}

			const size_t store_target = m_code.Size();
			return m_code.PatchBranch(store_after_overflow, store_target) &&
				   m_code.PatchBranch(no_underflow_from_exponent, store_target, VitaA32::Condition::NE) &&
				   m_code.PatchBranch(no_underflow_from_fraction, store_target, VitaA32::Condition::EQ) &&
				   store_result(store_fcr31);
		};

		const auto clear_invalid_divide_causes = [&]() {
			return m_code.EmitLdrImm12(HOST_TMP5, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   EmitBicImm32OrReg(HOST_TMP5, HOST_TMP5, FPU_FCR31_INVALID_DIVIDE_CAUSE_FLAGS, HOST_TMP2);
		};

		const auto emit_divide_by_zero_result = [&](unsigned exponent_mask_reg) {
			if (!m_code.EmitLdrImm12(HOST_TMP5, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, exponent_mask_reg) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t dividend_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (dividend_nonzero == static_cast<size_t>(-1))
				return false;

			if (!EmitOrrImm32OrReg(HOST_TMP5, HOST_TMP5, FPU_FCR31_INVALID_FLAGS, HOST_TMP4))
				return false;

			const size_t flags_ready = m_code.EmitBranchPlaceholder();
			if (flags_ready == static_cast<size_t>(-1))
				return false;

			const size_t dividend_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(dividend_nonzero, dividend_nonzero_target, VitaA32::Condition::NE) ||
				!EmitOrrImm32OrReg(HOST_TMP5, HOST_TMP5, FPU_FCR31_DIVIDE_BY_ZERO_FLAGS, HOST_TMP4))
			{
				return false;
			}

			const size_t flags_ready_target = m_code.Size();
			return m_code.PatchBranch(flags_ready, flags_ready_target) &&
				   m_code.EmitSubImm8(HOST_TMP3, exponent_mask_reg, 1) &&
				   m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) &&
				   EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP2) &&
				   m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP3) &&
				   store_result(true);
		};

		if (function == 0x03 && same_source) // DIV_S
		{
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
				!EmitCop1ExponentMask(HOST_TMP2) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t source_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (source_nonzero == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitLdrImm12(HOST_TMP5, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) ||
				!EmitOrrImm32OrReg(HOST_TMP5, HOST_TMP5, FPU_FCR31_INVALID_FLAGS, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP0, HOST_TMP2, 1) ||
				!store_result(true))
			{
				return false;
			}

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t source_nonzero_target = m_code.Size();
			// PCSX2 FPU.cpp::DIV_S() first rejects exponent-zero divisors.
			// Otherwise identical fpuDouble() inputs divide to +1 without
			// touching FCR31 because checkOverflow/Underflow receive flags=0.
			return m_code.PatchBranch(source_nonzero, source_nonzero_target, VitaA32::Condition::NE) &&
				   m_code.EmitMovImm32(HOST_TMP0, FPU_FLOAT_ONE) &&
				   store_result(false) &&
				   m_code.PatchBranch(done_from_zero, m_code.Size());
		}

		if (function == 0x03) // DIV_S
		{
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
				!EmitCop1ExponentMask(HOST_TMP2) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t divisor_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (divisor_nonzero == static_cast<size_t>(-1))
				return false;

			if (!emit_divide_by_zero_result(HOST_TMP2))
				return false;

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t divisor_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(divisor_nonzero, divisor_nonzero_target, VitaA32::Condition::NE))
			{
				return false;
			}

			if (!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP2, VitaA32::ShiftType::LSL, 0) ||
				!normalize_tracked_arithmetic_word_with_mask(HOST_TMP0, HOST_TMP5, IsCop1FprNormalized(fs)) ||
				!normalize_tracked_arithmetic_word_with_mask(HOST_TMP1, HOST_TMP5, IsCop1FprNormalized(ft)) ||
				!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) ||
				!m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1) ||
				!m_code.EmitVdivF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1) ||
				!m_code.EmitVmovSToCore(HOST_TMP0, VFP_FD_S2) ||
				!clamp_result_no_flags_with_mask(false, HOST_TMP5))
			{
				return false;
			}

			return m_code.PatchBranch(done_from_zero, m_code.Size());
		}

		if (function == 0x04) // SQRT_S
		{
			if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
				!clear_invalid_divide_causes() ||
				!EmitCop1ExponentMask(HOST_TMP2) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t operand_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (operand_nonzero == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP2))
			{
				return false;
			}

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t operand_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(operand_nonzero, operand_nonzero_target, VitaA32::Condition::NE) ||
				!EmitAndImm32OrReg(HOST_TMP3, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP4) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t operand_positive = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (operand_positive == static_cast<size_t>(-1))
				return false;

			if (!EmitOrrImm32OrReg(HOST_TMP5, HOST_TMP5, FPU_FCR31_INVALID_FLAGS, HOST_TMP4) ||
				!normalize_tracked_arithmetic_word_with_mask(HOST_TMP1, HOST_TMP2, IsCop1FprNormalized(ft)) ||
				!EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP4))
			{
				return false;
			}

			const size_t operand_ready = m_code.EmitBranchPlaceholder();
			if (operand_ready == static_cast<size_t>(-1))
				return false;

			const size_t operand_positive_target = m_code.Size();
			if (!m_code.PatchBranch(operand_positive, operand_positive_target, VitaA32::Condition::EQ) ||
				!normalize_tracked_arithmetic_word_with_mask(HOST_TMP1, HOST_TMP2, IsCop1FprNormalized(ft)))
			{
				return false;
			}

			const size_t operand_ready_target = m_code.Size();
			return m_code.PatchBranch(operand_ready, operand_ready_target) &&
				   m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1) &&
				   m_code.EmitVsqrtF32(VFP_FD_S2, VFP_FT_S1) &&
				   m_code.EmitVmovSToCore(HOST_TMP0, VFP_FD_S2) &&
				   m_code.PatchBranch(done_from_zero, m_code.Size()) &&
				   store_result(true);
		}

		if (function == 0x16) // RSQRT_S
		{
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
				!clear_invalid_divide_causes() ||
				!EmitCop1ExponentMask(HOST_TMP2) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t operand_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (operand_nonzero == static_cast<size_t>(-1))
				return false;

			if (!EmitOrrImm32OrReg(HOST_TMP5, HOST_TMP5, FPU_FCR31_DIVIDE_BY_ZERO_FLAGS, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP4, HOST_TMP2, 1) ||
				!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP2) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP4) ||
				!store_result(true))
			{
				return false;
			}

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t operand_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(operand_nonzero, operand_nonzero_target, VitaA32::Condition::NE) ||
				!EmitAndImm32OrReg(HOST_TMP3, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP4) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t operand_positive = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (operand_positive == static_cast<size_t>(-1))
				return false;

			if (!EmitOrrImm32OrReg(HOST_TMP5, HOST_TMP5, FPU_FCR31_INVALID_FLAGS, HOST_TMP4) ||
				!normalize_tracked_arithmetic_word_with_mask(HOST_TMP1, HOST_TMP2, IsCop1FprNormalized(ft)) ||
				!EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP4))
			{
				return false;
			}

			const size_t operand_ready = m_code.EmitBranchPlaceholder();
			if (operand_ready == static_cast<size_t>(-1))
				return false;

			const size_t operand_positive_target = m_code.Size();
			if (!m_code.PatchBranch(operand_positive, operand_positive_target, VitaA32::Condition::EQ) ||
				!normalize_tracked_arithmetic_word_with_mask(HOST_TMP1, HOST_TMP2, IsCop1FprNormalized(ft)))
			{
				return false;
			}

			const size_t operand_ready_target = m_code.Size();
			if (!m_code.PatchBranch(operand_ready, operand_ready_target) ||
				!normalize_tracked_arithmetic_word_with_mask(HOST_TMP0, HOST_TMP2, IsCop1FprNormalized(fs)) ||
				!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) ||
				!m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1) ||
				!m_code.EmitVsqrtF32(VFP_FT_S1, VFP_FT_S1) ||
				!m_code.EmitVdivF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1) ||
				!m_code.EmitVmovSToCore(HOST_TMP0, VFP_FD_S2) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP2, VitaA32::ShiftType::LSL, 0) ||
				!clamp_result_no_flags_with_mask(true, HOST_TMP3))
			{
				return false;
			}

			return m_code.PatchBranch(done_from_zero, m_code.Size());
		}

		return false;
	}

	bool BlockCompiler::EmitCOP1AccumulatorFast(u32 op)
	{
		// PCSX2 owners: FPU.cpp::ADDA_S()/SUBA_S()/MULA_S()/MADD_S()/
		// MSUB_S()/MADDA_S()/MSUBA_S(), plus FPU.cpp::fpuDouble(),
		// checkOverflow(), and checkUnderflow(). MADD/MSUB use the
		// FPU.cpp temporary product then fpuDouble() both ACC and the product.
		// MADDA/MSUBA keep FPU.cpp's raw ACC compound-assignment behavior.
		const unsigned fs = (op >> 11) & 0x1f;
		const unsigned ft = (op >> 16) & 0x1f;
		const unsigned fd = (op >> 6) & 0x1f;
		const u32 function = op & 0x3f;
		constexpr unsigned VFP_FS_S0 = 0;
		constexpr unsigned VFP_FT_S1 = 1;
		constexpr unsigned VFP_FD_S2 = 2;
		const bool same_source = fs == ft;
		const unsigned vfp_ft_source = same_source ? VFP_FS_S0 : VFP_FT_S1;

		const auto destination_offset = [&]() -> size_t {
			switch (function)
			{
				case 0x18: // ADDA_S
				case 0x19: // SUBA_S
				case 0x1a: // MULA_S
				case 0x1e: // MADDA_S
				case 0x1f: // MSUBA_S
					return FPU_ACC_OFFSET;
				default:
					return FprOffset(fd);
			}
		};

		const auto normalize_arithmetic_word = [&](unsigned reg) {
			if (!m_code.EmitAndReg(HOST_TMP3, reg, HOST_TMP5) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP5))
			{
				return false;
			}

			const size_t not_infinity_or_nan = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (not_infinity_or_nan == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP4, HOST_TMP5, 1) ||
				!m_code.EmitOrrReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_clamp = m_code.EmitBranchPlaceholder();
			if (done_from_clamp == static_cast<size_t>(-1))
				return false;

			const size_t finite_or_zero_target = m_code.Size();
			if (!m_code.PatchBranch(not_infinity_or_nan, finite_or_zero_target, VitaA32::Condition::NE) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t done_from_finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (done_from_finite == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP4))
			{
				return false;
			}

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_from_clamp, done_target) &&
				   m_code.PatchBranch(done_from_finite, done_target, VitaA32::Condition::NE);
		};
		const auto normalize_tracked_arithmetic_word = [&](unsigned reg, bool already_normalized) {
			if (already_normalized)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuCop1NormalizedOperandSkips++;
#endif
				return true;
			}
			return normalize_arithmetic_word(reg);
		};

		const auto apply_overflow_underflow_flags = [&](size_t dest_offset) {
			if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) ||
				!m_code.EmitBicImm32(HOST_TMP2, HOST_TMP0, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP5))
			{
				return false;
			}

			const size_t no_overflow = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_overflow == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP2) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP5, 1) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!EmitOrrImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_ARITHMETIC_OVERFLOW_FLAGS, HOST_TMP2))
			{
				return false;
			}

			const size_t store_result = m_code.EmitBranchPlaceholder();
			if (store_result == static_cast<size_t>(-1))
				return false;

			const size_t no_overflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_overflow, no_overflow_target, VitaA32::Condition::NE) ||
				!EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_OVERFLOW_FLAG, HOST_TMP2) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP5) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t no_underflow_from_exponent = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_underflow_from_exponent == static_cast<size_t>(-1))
				return false;

			if (!EmitAndCop1FractionMask(HOST_TMP3, HOST_TMP0) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t no_underflow_from_fraction = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (no_underflow_from_fraction == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP2) ||
				!EmitOrrImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_ARITHMETIC_UNDERFLOW_FLAGS, HOST_TMP2))
			{
				return false;
			}

			const size_t underflow_store = m_code.EmitBranchPlaceholder();
			if (underflow_store == static_cast<size_t>(-1))
				return false;

			const size_t clear_underflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_underflow_from_exponent, clear_underflow_target, VitaA32::Condition::NE) ||
				!m_code.PatchBranch(no_underflow_from_fraction, clear_underflow_target, VitaA32::Condition::EQ) ||
				!EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_UNDERFLOW_FLAG, HOST_TMP2))
			{
				return false;
			}

			const size_t store_target = m_code.Size();
			return m_code.PatchBranch(store_result, store_target) &&
				   m_code.PatchBranch(underflow_store, store_target) &&
				   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(dest_offset));
		};

		const auto store_positive_zero_and_clear_overflow_underflow = [&](size_t dest_offset) {
			return m_code.EmitMovImm8(HOST_TMP0, 0) &&
				   m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_OVERFLOW_UNDERFLOW_FLAGS, HOST_TMP2) &&
				   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(dest_offset));
		};

		const auto load_normalized_operands = [&]() {
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) &&
				   EmitCop1ExponentMask(HOST_TMP5) &&
				   normalize_tracked_arithmetic_word(HOST_TMP0, IsCop1FprNormalized(fs)) &&
				   m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) &&
				   (same_source ||
					   (m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) &&
						   normalize_tracked_arithmetic_word(HOST_TMP1, IsCop1FprNormalized(ft)) &&
						   m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1)));
		};

		if (function == 0x19 && fs == ft) // SUBA_S
		{
			// PCSX2 FPU.cpp::fpuDouble() normalizes both operands identically
			// for same-FPR SUBA_S, producing +0 in ACC with O/U causes cleared.
			return store_positive_zero_and_clear_overflow_underflow(destination_offset());
		}

		if (!load_normalized_operands())
			return false;

		switch (function)
		{
			case 0x18: // ADDA_S
				if (!m_code.EmitVaddF32(VFP_FD_S2, VFP_FS_S0, vfp_ft_source))
					return false;
				break;
			case 0x19: // SUBA_S
				if (!m_code.EmitVsubF32(VFP_FD_S2, VFP_FS_S0, vfp_ft_source))
					return false;
				break;
			case 0x1a: // MULA_S
			case 0x1c: // MADD_S
			case 0x1d: // MSUB_S
			case 0x1e: // MADDA_S
			case 0x1f: // MSUBA_S
				if (!m_code.EmitVmulF32(VFP_FD_S2, VFP_FS_S0, vfp_ft_source))
					return false;
				break;
			default:
				return false;
		}

		if (function == 0x1c || function == 0x1d) // MADD_S / MSUB_S
		{
			if (!m_code.EmitVmovSToCore(HOST_TMP0, VFP_FD_S2) ||
				!normalize_arithmetic_word(HOST_TMP0) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FPU_ACC_OFFSET)) ||
				!normalize_tracked_arithmetic_word(HOST_TMP1, IsCop1AccNormalized()) ||
				!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP1) ||
				!m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP0))
			{
				return false;
			}

			if (function == 0x1c)
			{
				if (!m_code.EmitVaddF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1))
					return false;
			}
			else if (!m_code.EmitVsubF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1))
			{
				return false;
			}
		}
		else if (function == 0x1e || function == 0x1f) // MADDA_S / MSUBA_S
		{
			if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FPU_ACC_OFFSET)) ||
				!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP1))
			{
				return false;
			}

			if (function == 0x1e)
			{
				if (!m_code.EmitVaddF32(VFP_FD_S2, VFP_FS_S0, VFP_FD_S2))
					return false;
			}
			else if (!m_code.EmitVsubF32(VFP_FD_S2, VFP_FS_S0, VFP_FD_S2))
			{
				return false;
			}
		}

		return m_code.EmitVmovSToCore(HOST_TMP0, VFP_FD_S2) &&
			   apply_overflow_underflow_flags(destination_offset());
	}

	bool BlockCompiler::EmitCOP1ScalarWordFast(u32 op)
	{
		// PCSX2 owners: FPU.cpp::MOV_S()/ABS_S()/NEG_S()/MAX_S()/MIN_S().
		// These operate on fpuRegs.fpr[] as 32-bit words; ABS_S, NEG_S, MAX_S,
		// and MIN_S also clear only FPUflagO | FPUflagU in fpuRegs.fprc[31].
		const unsigned fs = (op >> 11) & 0x1f;
		const unsigned ft = (op >> 16) & 0x1f;
		const unsigned fd = (op >> 6) & 0x1f;
		const u32 function = op & 0x3f;

		const auto clear_overflow_underflow = [&]() {
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   EmitBicImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FCR31_OVERFLOW_UNDERFLOW_FLAGS, HOST_TMP1) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31)));
		};

		if (function == 0x06 && fd == fs) // MOV_S
			return true;

		if ((function == 0x28 || function == 0x29) && fs == ft) // MAX_S / MIN_S
		{
			// PCSX2 FPU.cpp::fp_max()/fp_min() return the identical source word
			// for same-FPR operands; MAX_S/MIN_S still clear O/U causes.
			if (fd != fs &&
				(!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
					!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd)))))
			{
				return false;
			}
			return clear_overflow_underflow();
		}

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))))
			return false;

		switch (function)
		{
			case 0x05: // ABS_S
				if (!m_code.EmitBicImm32(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK))
	{
					return false;
	}
				break;
			case 0x06: // MOV_S
				break;
			case 0x07: // NEG_S
				if (!EmitEorImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP1))
	{
					return false;
	}
				break;
			case 0x28: // MAX_S
			case 0x29: // MIN_S
			{
				// PCSX2 FPU.cpp::fp_max()/fp_min() use signed word ordering and
				// reverse the comparison only when both operands are negative.
				const bool max_op = function == 0x28;
				const VitaA32::Condition normal_select_ft =
					max_op ? VitaA32::Condition::LT : VitaA32::Condition::GT;
				const VitaA32::Condition negative_select_ft =
					max_op ? VitaA32::Condition::GT : VitaA32::Condition::LT;

				if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
					!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSL, 0,
						false, normal_select_ft) ||
					!m_code.EmitAndReg(HOST_TMP4, HOST_TMP0, HOST_TMP1) ||
					!EmitAndImm32OrReg(HOST_TMP4, HOST_TMP4, FPU_FLOAT_SIGN_MASK, HOST_TMP3) ||
					!m_code.EmitCmpImm32(HOST_TMP4, FPU_FLOAT_SIGN_MASK))
	{
					return false;
	}

				const size_t store_word = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
				if (store_word == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
					!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSL, 0,
						false, negative_select_ft) ||
					!m_code.PatchBranch(store_word, m_code.Size(), VitaA32::Condition::NE) ||
					!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd))))
	{
					return false;
	}

				return clear_overflow_underflow();
			}
			default:
				return false;
		}

		if (!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd))))
			return false;

		if (function == 0x06)
			return true;

		return clear_overflow_underflow();
	}

	bool BlockCompiler::EmitCOP1CompareFast(u32 op)
	{
		// PCSX2 owners: FPU.cpp::C_F()/C_EQ()/C_LT()/C_LE() and
		// FPU.cpp::fpuDouble(). fpuDouble() clamps all exponent-0 values to
		// signed zero and all exponent-0xff values to signed max finite before
		// comparing, so this path does the same as integer transforms.
		const unsigned fs = (op >> 11) & 0x1f;
		const unsigned ft = (op >> 16) & 0x1f;
		const u32 function = op & 0x3f;

		const auto clear_condition_flag = [&]() {
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   EmitBicImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FCR31_CONDITION_FLAG, HOST_TMP1) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31)));
		};
		const auto set_condition_flag = [&]() {
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   EmitOrrImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FCR31_CONDITION_FLAG, HOST_TMP1) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31)));
		};

		const auto normalize_compare_word = [&](unsigned reg) {
			if (!m_code.EmitAndReg(HOST_TMP3, reg, HOST_TMP5) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP5))
			{
				return false;
			}

			const size_t not_infinity_or_nan = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (not_infinity_or_nan == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP4, HOST_TMP5, 1) ||
				!m_code.EmitOrrReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_clamp = m_code.EmitBranchPlaceholder();
			if (done_from_clamp == static_cast<size_t>(-1))
				return false;

			const size_t finite_or_zero_target = m_code.Size();
			if (!m_code.PatchBranch(not_infinity_or_nan, finite_or_zero_target, VitaA32::Condition::NE) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t done_from_finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (done_from_finite == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm8(reg, 0))
				return false;

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_from_clamp, done_target) &&
				   m_code.PatchBranch(done_from_finite, done_target, VitaA32::Condition::NE);
		};

		const auto make_sortable_compare_key = [&](unsigned reg) {
			return m_code.EmitMovRegShiftImm(HOST_TMP2, reg, VitaA32::ShiftType::ASR, 31) &&
				   EmitOrrImm32OrReg(HOST_TMP2, HOST_TMP2, FPU_FLOAT_SIGN_MASK, HOST_TMP3) &&
				   m_code.EmitEorReg(reg, reg, HOST_TMP2);
		};

		const auto inverse_condition = [](VitaA32::Condition condition, VitaA32::Condition* inverse) {
			switch (condition)
			{
				case VitaA32::Condition::EQ:
					*inverse = VitaA32::Condition::NE;
					return true;
				case VitaA32::Condition::CC:
					*inverse = VitaA32::Condition::CS;
					return true;
				case VitaA32::Condition::LS:
					*inverse = VitaA32::Condition::HI;
					return true;
				default:
					return false;
			}
		};

		const auto apply_condition_flag = [&](VitaA32::Condition condition) {
			VitaA32::Condition clear_only_condition = VitaA32::Condition::AL;
			if (!inverse_condition(condition, &clear_only_condition) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) ||
				!EmitBicImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FCR31_CONDITION_FLAG, HOST_TMP2))
			{
				return false;
			}

			const size_t clear_only = m_code.EmitBranchPlaceholder(clear_only_condition);
			if (clear_only == static_cast<size_t>(-1))
				return false;

			if (!EmitOrrImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FCR31_CONDITION_FLAG, HOST_TMP1))
				return false;

			const size_t store_target = m_code.Size();
			return m_code.PatchBranch(clear_only, store_target, clear_only_condition) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31)));
		};

		if (function == 0x30) // C_F
			return clear_condition_flag();
		if (fs == ft)
		{
			// PCSX2 FPU.cpp::fpuDouble() maps one source bit-pattern identically
			// on both sides, including exponent-0 and exponent-0xff clamps.
			if (function == 0x32 || function == 0x36) // C_EQ / C_LE
				return set_condition_flag();
			if (function == 0x34) // C_LT
				return clear_condition_flag();
		}

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
			!EmitCop1ExponentMask(HOST_TMP5) ||
			!normalize_compare_word(HOST_TMP0) ||
			!normalize_compare_word(HOST_TMP1))
		{
			return false;
		}

		switch (function)
		{
			case 0x32: // C_EQ
				return m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) &&
					   apply_condition_flag(VitaA32::Condition::EQ);
			case 0x34: // C_LT
				return make_sortable_compare_key(HOST_TMP0) &&
					   make_sortable_compare_key(HOST_TMP1) &&
					   m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) &&
					   apply_condition_flag(VitaA32::Condition::CC);
			case 0x36: // C_LE
				return make_sortable_compare_key(HOST_TMP0) &&
					   make_sortable_compare_key(HOST_TMP1) &&
					   m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) &&
					   apply_condition_flag(VitaA32::Condition::LS);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitCOP1ConvertWordFast(u32 op)
	{
		// PCSX2 owner: FPU.cpp::CVT_W(). PCSX2 saturates when the exponent mask
		// exceeds 0x4e800000, otherwise the float is in signed-int range and the
		// C++ cast truncates toward zero. This integer path mirrors that gate.
		const unsigned fs = (op >> 11) & 0x1f;
		const unsigned fd = (op >> 6) & 0x1f;

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
			!EmitAndCop1ExponentMask(HOST_TMP2, HOST_TMP0, HOST_TMP1) ||
			!EmitCmpImm32OrReg(HOST_TMP2, FPU_CVT_W_MAX_EXPONENT_MASK, HOST_TMP3))
		{
			return false;
		}

		const size_t convert_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::LS);
		if (convert_path == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitTstImm32(HOST_TMP0, FPU_FLOAT_SIGN_MASK) ||
			!m_code.EmitMovImm32(HOST_TMP0, 0x7fffffffu) ||
			!m_code.EmitMovImm32(HOST_TMP0, FPU_FLOAT_SIGN_MASK, VitaA32::Condition::NE))
		{
			return false;
		}

		const size_t store_result = m_code.EmitBranchPlaceholder();
		if (store_result == static_cast<size_t>(-1))
			return false;

		const size_t convert_target = m_code.Size();
		if (!m_code.PatchBranch(convert_path, convert_target, VitaA32::Condition::LS) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSR, 23) ||
			!EmitCmpImm32OrReg(HOST_TMP2, FPU_FLOAT_EXPONENT_BIAS, HOST_TMP3))
		{
			return false;
		}

		const size_t nonzero_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
		if (nonzero_path == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitMovImm8(HOST_TMP0, 0))
			return false;

		const size_t zero_done = m_code.EmitBranchPlaceholder();
		if (zero_done == static_cast<size_t>(-1))
			return false;

		const size_t nonzero_target = m_code.Size();
		if (!m_code.PatchBranch(nonzero_path, nonzero_target, VitaA32::Condition::CS) ||
			!EmitAndCop1FractionMask(HOST_TMP3, HOST_TMP0) ||
			!EmitOrrImm32OrReg(HOST_TMP3, HOST_TMP3, FPU_FLOAT_IMPLICIT_MANTISSA, HOST_TMP4) ||
			!m_code.EmitMovImm8(HOST_TMP4, FPU_FLOAT_EXPONENT_BIAS) ||
			!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
			!m_code.EmitCmpImm32(HOST_TMP2, FPU_FLOAT_MANTISSA_BITS))
		{
			return false;
		}

		const size_t left_shift_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
		if (left_shift_path == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP2, FPU_FLOAT_MANTISSA_BITS) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_TMP3, VitaA32::ShiftType::LSR, HOST_TMP4))
		{
			return false;
		}

		const size_t apply_sign = m_code.EmitBranchPlaceholder();
		if (apply_sign == static_cast<size_t>(-1))
			return false;

		const size_t left_shift_target = m_code.Size();
		if (!m_code.PatchBranch(left_shift_path, left_shift_target, VitaA32::Condition::CS) ||
			!m_code.EmitSubImm32(HOST_TMP4, HOST_TMP2, FPU_FLOAT_MANTISSA_BITS) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_TMP3, VitaA32::ShiftType::LSL, HOST_TMP4))
		{
			return false;
		}

		const size_t apply_sign_target = m_code.Size();
		if (!m_code.PatchBranch(apply_sign, apply_sign_target) ||
			!EmitAndImm32OrReg(HOST_TMP4, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP1, true) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP3, VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}

		const size_t positive_done = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (positive_done == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitRsbImm32(HOST_TMP0, HOST_TMP3, 0))
		{
			return false;
		}

		const size_t final_store = m_code.Size();
		return m_code.PatchBranch(store_result, final_store) &&
			   m_code.PatchBranch(zero_done, final_store) &&
			   m_code.PatchBranch(positive_done, final_store, VitaA32::Condition::EQ) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd)));
	}

	bool BlockCompiler::EmitCOP1ConvertSingleFast(u32 op)
	{
		// PCSX2 owner: FPU.cpp::CVT_S(). PCSX2 casts the signed source FPR word
		// to float and does not update FCR31, so the Cortex-A9 path uses
		// call-clobbered VFP s0 for the same int-to-single conversion.
		const unsigned fs = (op >> 11) & 0x1f;
		const unsigned fd = (op >> 6) & 0x1f;
		constexpr unsigned VFP_TMP_S0 = 0;

		return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) &&
			   m_code.EmitVmovCoreToS(VFP_TMP_S0, HOST_TMP0) &&
			   m_code.EmitVcvtF32S32(VFP_TMP_S0, VFP_TMP_S0) &&
			   m_code.EmitVmovSToCore(HOST_TMP0, VFP_TMP_S0) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd)));
	}

	bool BlockCompiler::EmitCACHE(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		(void)pc;
		(void)raw_cycles_through_instruction;
		(void)event_exit;

		if (IsNoOpCACHE(op))
			return true;

		if (!IsHelperCACHE(op))
			return false;

		const s16 imm = IMM_S(op);
		const auto emit_addr_adjust = [&]() {
			if (imm == 0)
				return true;
			if (imm > 0)
			{
				const u32 delta = static_cast<u32>(imm);
				return m_code.EmitAddImm32(HOST_TMP1, HOST_TMP1, delta) ||
					   (m_code.EmitMovImm32(HOST_TMP2, delta) &&
					    m_code.EmitAddReg(HOST_TMP1, HOST_TMP1, HOST_TMP2));
			}

			const u32 delta = static_cast<u32>(-static_cast<s32>(imm));
			return m_code.EmitSubImm32(HOST_TMP1, HOST_TMP1, delta) ||
				   (m_code.EmitMovImm32(HOST_TMP2, delta) &&
				    m_code.EmitSubReg(HOST_TMP1, HOST_TMP1, HOST_TMP2));
		};

		// PCSX2 owner: Cache.cpp::CACHE()/executeCacheOp(). Compute the CACHE
		// address in A32 and jump straight to the owner operation, avoiding the
		// interpreter's cpuRegs.code store/decode while preserving TagLo/cache lines.
		return EmitLoadGprLow(RS(op), HOST_TMP1) &&
			   emit_addr_adjust() &&
			   m_code.EmitMovImm32(HOST_TMP0, op) &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&executeCacheOp));
	}

	bool BlockCompiler::EmitSpecialExceptionEventExit(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot, const void* helper)
	{
		if (!event_exit || !helper || raw_cycles_through_instruction == 0)
			return false;

		// The exception helpers read the GPR file (e.g. SYSCALL's $v1), so
		// deferred pinned words must be resident in backing before the call.
		const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
		if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
			!EmitSyncGprPinsToBacking() ||
			!EmitStorePc(pc + 4) ||
			!m_code.EmitMovImm8(HOST_TMP0, branch_delay_slot ? 1 : 0) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(BRANCH_OFFSET)) ||
			!EmitAddScaledCyclesToCpu(cycles) ||
			!m_code.EmitCallAbsolute(helper))
		{
			return false;
		}

		return EmitEventExitReturn(event_exit);
	}

	bool BlockCompiler::EmitSYSCALL(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		using namespace R5900::Interpreter::OpcodeImpl;
		return EmitSpecialExceptionEventExit(op, pc, raw_cycles_through_instruction,
			event_exit, branch_delay_slot, reinterpret_cast<const void*>(&SYSCALL));
	}

	bool BlockCompiler::EmitBREAK(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		using namespace R5900::Interpreter::OpcodeImpl;
		return EmitSpecialExceptionEventExit(op, pc, raw_cycles_through_instruction,
			event_exit, branch_delay_slot, reinterpret_cast<const void*>(&BREAK));
	}

	bool BlockCompiler::EmitTrapEventExit(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::TGE/TGEU/TLT/TLTU/TEQ/TNE and
		// TGEI/TGEIU/TLTI/TLTIU/TEQI/TNEI. The x86 recompiler keeps these as
		// recBranchCall() helper tails, so the first A32 port does the same.
		return EmitSpecialExceptionEventExit(op, pc, raw_cycles_through_instruction,
			event_exit, branch_delay_slot, TrapHelperForOpcode(op));
	}

	bool BlockCompiler::EmitADDIU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const s32 imm = static_cast<s32>(IMM_S(op));

		if (rt == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::ADDIU() sign-extends the 32-bit
		// result of RS.low + sign_extend_16(imm). With RS=$zero that is just
		// the sign-extended immediate.
		if (rs == 0)
			return EmitStoreKnownSignExtended32(rt, static_cast<u32>(imm));

		u32 rs_value = 0;
		if (FindGprPinHost(rs) < 0 && TryGetKnownGprLow(rs, &rs_value))
		{
			// PCSX2 x86/ix86-32/iR5900AritImm.cpp::recADDI_const() folds this
			// as a signed-extended 32-bit result; do the same before touching
			// cpuRegs on Cortex-A9, but only when it replaces a real load. If
			// the pin cache already has RS resident, register ADD/SUB is cheaper
			// than rematerializing the full result.
			return EmitStoreKnownSignExtended32(rt, rs_value + static_cast<u32>(imm));
		}

		unsigned rs_host;
		if (!EmitGprLowOperand(rs, HOST_TMP0, &rs_host))
			return false;

		const unsigned result_reg = SelectGprLowResultHost(rt, HOST_TMP0);
		const bool encoded_imm = (imm >= 0 && m_code.EmitAddImm32(result_reg, rs_host, static_cast<u32>(imm))) ||
								 (imm < 0 && m_code.EmitSubImm32(result_reg, rs_host, static_cast<u32>(-imm)));
		if (!encoded_imm)
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, static_cast<u32>(imm)) ||
				!m_code.EmitAddReg(result_reg, rs_host, HOST_TMP2))
			{
				return false;
			}
		}

		return EmitStoreGprSignExtended32FromLow(rt, result_reg);
	}

	bool BlockCompiler::EmitDADDIU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const s32 imm = static_cast<s32>(IMM_S(op));

		if (rt == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::DADDIU() sign-extends the 16-bit
		// immediate and adds it to the low 64-bit GPR value. With RS=$zero the
		// result is exactly that sign-extended immediate.
		if (rs == 0)
			return EmitStoreKnown64(rt, static_cast<u32>(imm), (imm < 0) ? 0xffffffffu : 0);
		if (imm == 0 && rt == rs)
			return true;

		u32 rs_low_value = 0;
		u32 rs_high_value = 0;
		if (FindGprPinHost(rs) < 0 && TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value))
		{
			// PCSX2 keeps full-width GPR constants in g_cpuConstRegs; fold the
			// same DADDIU low-64 add when the exact high word is proven.
			const u64 rs_value = (static_cast<u64>(rs_high_value) << 32) | rs_low_value;
			const u64 result = rs_value + static_cast<u64>(static_cast<s64>(imm));
			return EmitStoreKnown64(rt, static_cast<u32>(result), static_cast<u32>(result >> 32));
		}

		if (imm == 0)
		{
			bool qcache_store = false;
			if (!TryEmitStoreGprLow64FromQCache(rt, rs, &qcache_store))
				return false;
			if (qcache_store)
				return true;
		}

		unsigned rs_low;
		unsigned rs_high;
		if (!EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high))
			return false;

		// PCSX2 owner: R5900OpcodeImpl.cpp::DADDIU() sign-extends the 16-bit
		// immediate and performs the 64-bit add without overflow trapping.
		if (imm == 0)
			return EmitStoreGpr64(rt, rs_low, rs_high);

		const unsigned low_result = SelectGprLowResultHost(rt, HOST_TMP0);
		const unsigned high_result = SelectGprHighResultHost(rt, HOST_TMP1);
		if (imm > 0 && m_code.EmitAddImm32(low_result, rs_low, static_cast<u32>(imm), true))
			return m_code.EmitAdcImm8(high_result, rs_high, 0) &&
				   EmitStoreGpr64(rt, low_result, high_result);

		if (imm < 0 && m_code.EmitSubImm32(low_result, rs_low, static_cast<u32>(-imm), true))
			return m_code.EmitSbcImm8(high_result, rs_high, 0) &&
				   EmitStoreGpr64(rt, low_result, high_result);

		return m_code.EmitMovImm32(HOST_TMP2, static_cast<u32>(imm)) &&
			   m_code.EmitAddReg(low_result, rs_low, HOST_TMP2, true) &&
			   m_code.EmitMovImm32(HOST_TMP2, (imm < 0) ? 0xffffffffu : 0) &&
			   m_code.EmitAdcReg(high_result, rs_high, HOST_TMP2) &&
			   EmitStoreGpr64(rt, low_result, high_result);
	}

	bool BlockCompiler::EmitSLTI(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const s32 imm = static_cast<s32>(IMM_S(op));

		if (rt == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLTI(). With RS=$zero, the
		// signed comparison is the constant predicate 0 < sign_extend_16(imm).
		if (rs == 0)
		{
			const u32 result = (imm > 0) ? 1u : 0u;
			return EmitStoreKnownZeroExtended32(rt, result);
		}

		u32 rs_low_value = 0;
		u32 rs_high_value = 0;
		if (TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value))
		{
			// PCSX2 x86/ix86-32/iR5900AritImm.cpp::recSLTI_const() folds the
			// full SD[0] compare through GPR_IS_CONST1.
			const u64 rs_value = (static_cast<u64>(rs_high_value) << 32) | rs_low_value;
			const bool result = static_cast<s64>(rs_value) < static_cast<s64>(imm);
			return EmitStoreKnownZeroExtended32(rt, result ? 1 : 0);
		}

		unsigned rs_low;
		unsigned rs_high;
		return EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high) &&
			   EmitSetLessThan64Imm(rt, imm, true, rs_low, rs_high);
	}

	bool BlockCompiler::EmitSLTIU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const s32 imm = static_cast<s32>(IMM_S(op));

		if (rt == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLTIU(). The immediate is
		// sign-extended before the unsigned 64-bit compare, so 0 is less than
		// every nonzero immediate.
		if (rs == 0)
		{
			const u32 result = (imm != 0) ? 1u : 0u;
			return EmitStoreKnownZeroExtended32(rt, result);
		}

		u32 rs_low_value = 0;
		u32 rs_high_value = 0;
		if (TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value))
		{
			// PCSX2 x86/ix86-32/iR5900AritImm.cpp::recSLTIU_const() folds the
			// full UD[0] compare through GPR_IS_CONST1.
			const u64 rs_value = (static_cast<u64>(rs_high_value) << 32) | rs_low_value;
			const u64 imm_value = (static_cast<u64>((imm < 0) ? 0xffffffffu : 0) << 32) |
								  static_cast<u32>(imm);
			return EmitStoreKnownZeroExtended32(rt, rs_value < imm_value ? 1 : 0);
		}

		unsigned rs_low;
		unsigned rs_high;
		return EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high) &&
			   EmitSetLessThan64Imm(rt, imm, false, rs_low, rs_high);
	}

	bool BlockCompiler::EmitANDI(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const u16 imm = IMM_U(op);

		if (rt == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::ANDI(); result is zero-extended into the low 64 bits.
		if (rs == 0 || imm == 0)
			return EmitStoreGprZero64(rt);

		u32 rs_value = 0;
		if (FindGprPinHost(rs) < 0 && TryGetKnownGprLow(rs, &rs_value))
		{
			// PCSX2 R5900OpcodeImpl.cpp::ANDI() zero-extends the 16-bit mask
			// into the low 64-bit lane, so the block-local low32 fact is enough.
			return EmitStoreKnownZeroExtended32(rt, rs_value & imm);
		}

		unsigned rs_host;
		if (!EmitGprLowOperand(rs, HOST_TMP0, &rs_host))
			return false;

		const unsigned result_reg = SelectGprLowResultHost(rt, HOST_TMP0);
		return EmitAndImm32OrReg(result_reg, rs_host, imm, HOST_TMP2) &&
			   EmitStoreGprZeroExtended32FromLow(rt, result_reg);
	}

	bool BlockCompiler::EmitORI(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const u16 imm = IMM_U(op);

		if (rt == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::ORI(); the immediate is zero-extended,
		// while the source high word of the low 64-bit lane is preserved.
		if (imm == 0 && rt == rs)
			return true;

		if (rs == 0)
			return EmitStoreKnownZeroExtended32(rt, imm);

		if (imm == 0)
		{
			bool qcache_store = false;
			if (!TryEmitStoreGprLow64FromQCache(rt, rs, &qcache_store))
				return false;
			if (qcache_store)
				return true;
		}

		u32 rs_low_value = 0;
		u32 rs_high_value = 0;
		if (FindGprPinHost(rs) < 0 && TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value))
		{
			// PCSX2 x86/ix86-32/iR5900AritImm.cpp::recORI_const() folds the
			// full low-64 constant; require the exact high-word proof because
			// ORI preserves source bits above the zero-extended immediate.
			return EmitStoreKnown64(rt, rs_low_value | imm, rs_high_value);
		}

		unsigned rs_low;
		unsigned rs_high;
		if (!EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high))
			return false;

		const unsigned result_reg = SelectGprLowResultHost(rt, HOST_TMP0);
		if (!m_code.EmitOrrImm32(result_reg, rs_low, imm))
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, imm) ||
				!m_code.EmitOrrReg(result_reg, rs_low, HOST_TMP2))
			{
				return false;
			}
		}

		return EmitStoreGpr64(rt, result_reg, rs_high);
	}

	bool BlockCompiler::EmitXORI(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const u16 imm = IMM_U(op);

		if (rt == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::XORI(); the immediate is zero-extended,
		// while the source high word of the low 64-bit lane is preserved.
		if (imm == 0 && rt == rs)
			return true;

		if (rs == 0)
			return EmitStoreKnownZeroExtended32(rt, imm);

		if (imm == 0)
		{
			bool qcache_store = false;
			if (!TryEmitStoreGprLow64FromQCache(rt, rs, &qcache_store))
				return false;
			if (qcache_store)
				return true;
		}

		u32 rs_low_value = 0;
		u32 rs_high_value = 0;
		if (FindGprPinHost(rs) < 0 && TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value))
		{
			// PCSX2 x86/ix86-32/iR5900AritImm.cpp::recXORI_const() folds the
			// full low-64 constant; require the exact high-word proof because
			// XORI preserves source bits above the zero-extended immediate.
			return EmitStoreKnown64(rt, rs_low_value ^ imm, rs_high_value);
		}

		unsigned rs_low;
		unsigned rs_high;
		if (!EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high))
			return false;

		const unsigned result_reg = SelectGprLowResultHost(rt, HOST_TMP0);
		if (!m_code.EmitEorImm32(result_reg, rs_low, imm))
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, imm) ||
				!m_code.EmitEorReg(result_reg, rs_low, HOST_TMP2))
			{
				return false;
			}
		}

		return EmitStoreGpr64(rt, result_reg, rs_high);
	}

	bool BlockCompiler::EmitLUI(u32 op)
	{
		const unsigned rt = RT(op);
		if (rt == 0)
			return true;

		const u32 value = op << 16;
		return EmitStoreKnownSignExtended32(rt, value);
	}

	bool BlockCompiler::EmitSLL(u32 op)
	{
		return EmitShift32Immediate(op, VitaA32::ShiftType::LSL, SA(op));
	}

	bool BlockCompiler::EmitSRL(u32 op)
	{
		return EmitShift32Immediate(op, VitaA32::ShiftType::LSR, SA(op));
	}

	bool BlockCompiler::EmitSRA(u32 op)
	{
		return EmitShift32Immediate(op, VitaA32::ShiftType::ASR, SA(op));
	}

	bool BlockCompiler::EmitSLLV(u32 op)
	{
		return EmitShift32Variable(op, VitaA32::ShiftType::LSL);
	}

	bool BlockCompiler::EmitSRLV(u32 op)
	{
		return EmitShift32Variable(op, VitaA32::ShiftType::LSR);
	}

	bool BlockCompiler::EmitSRAV(u32 op)
	{
		return EmitShift32Variable(op, VitaA32::ShiftType::ASR);
	}

	bool BlockCompiler::EmitMOVZ(u32 op)
	{
		return EmitConditionalMove(op, true);
	}

	bool BlockCompiler::EmitMOVN(u32 op)
	{
		return EmitConditionalMove(op, false);
	}

	bool BlockCompiler::EmitMULT(u32 op)
	{
		return EmitMultiply(op, true, false);
	}

	bool BlockCompiler::EmitMULTU(u32 op)
	{
		return EmitMultiply(op, false, false);
	}

	bool BlockCompiler::EmitMADD(u32 op)
	{
		return EmitMultiplyAdd(op, true, false);
	}

	bool BlockCompiler::EmitMADDU(u32 op)
	{
		return EmitMultiplyAdd(op, false, false);
	}

	bool BlockCompiler::EmitMADD1(u32 op)
	{
		return EmitMultiplyAdd(op, true, true);
	}

	bool BlockCompiler::EmitMADDU1(u32 op)
	{
		return EmitMultiplyAdd(op, false, true);
	}

	bool BlockCompiler::EmitMFHI1(u32 op)
	{
		// PCSX2 owner: MMI.cpp::MFHI1().
		return EmitMoveFromHiLo(op, HiloLaneOffset(HI_OFFSET, true));
	}

	bool BlockCompiler::EmitMFLO1(u32 op)
	{
		// PCSX2 owner: MMI.cpp::MFLO1().
		return EmitMoveFromHiLo(op, HiloLaneOffset(LO_OFFSET, true));
	}

	bool BlockCompiler::EmitMTHI1(u32 op)
	{
		// PCSX2 owner: MMI.cpp::MTHI1().
		return EmitMoveToHiLo(op, HiloLaneOffset(HI_OFFSET, true));
	}

	bool BlockCompiler::EmitMTLO1(u32 op)
	{
		// PCSX2 owner: MMI.cpp::MTLO1().
		return EmitMoveToHiLo(op, HiloLaneOffset(LO_OFFSET, true));
	}

	bool BlockCompiler::EmitMULT1(u32 op)
	{
		return EmitMultiply(op, true, true);
	}

	bool BlockCompiler::EmitMULTU1(u32 op)
	{
		return EmitMultiply(op, false, true);
	}

	bool BlockCompiler::EmitMultiply(u32 op, bool signed_multiply, bool upper_pipeline)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::MULT()/MULTU(), MMI.cpp::MULT1()/MULTU1(),
		// and x86/ix86-32/iR5900MultDiv.cpp::recMULT*(). All forms sign-extend
		// the 32-bit LO/HI halves into their 64-bit lanes, and the EE
		// three-operand form writes the selected LO lane into rd as well.
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		const size_t lo_offset = HiloLaneOffset(LO_OFFSET, upper_pipeline);
		const size_t hi_offset = HiloLaneOffset(HI_OFFSET, upper_pipeline);

		unsigned rs_host;
		unsigned rt_host;
		if (!EmitGprLowOperand(rs, HOST_TMP0, &rs_host) ||
			!EmitGprLowOperand(rt, HOST_TMP1, &rt_host))
		{
			return false;
		}

		unsigned product_low = HOST_TMP2;
		if (rd != 0)
		{
			const int low_pin = FindGprPinHost(rd);
			if (low_pin >= 0)
			{
				const unsigned low_candidate = static_cast<unsigned>(low_pin);
				if (low_candidate != rs_host && low_candidate != rt_host && low_candidate != HOST_TMP3)
	{
					product_low = low_candidate;
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuGprPinLowResultStoreOperands++;
#endif
	}
			}
		}

		if (signed_multiply)
		{
			if (!m_code.EmitSmull(product_low, HOST_TMP3, rs_host, rt_host))
				return false;
		}
		else
		{
			if (!m_code.EmitUmull(product_low, HOST_TMP3, rs_host, rt_host))
				return false;
		}

		unsigned rd_high = HOST_TMP0;
		if (rd != 0)
		{
			const int high_pin = FindGprPinHighHost(rd);
			if (high_pin >= 0)
			{
				const unsigned high_candidate = static_cast<unsigned>(high_pin);
				if (high_candidate != product_low && high_candidate != HOST_TMP3)
					rd_high = high_candidate;
			}
		}

		if (!m_code.EmitMovRegShiftImm(rd_high, product_low, VitaA32::ShiftType::ASR, 31) ||
			!m_code.EmitStrImm12(product_low, HOST_CPU_REGS, static_cast<u16>(lo_offset)) ||
			!m_code.EmitStrImm12(rd_high, HOST_CPU_REGS, static_cast<u16>(lo_offset + sizeof(u32))))
		{
			return false;
		}

		if (rd != 0 && !EmitStoreGpr64(rd, product_low, rd_high))
			return false;

		return m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP3, VitaA32::ShiftType::ASR, 31) &&
			   m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(hi_offset)) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(hi_offset + sizeof(u32)));
	}

	bool BlockCompiler::EmitMultiplyAdd(u32 op, bool signed_multiply, bool upper_pipeline)
	{
		// PCSX2 owners: MMI.cpp::MADD()/MADDU()/MADD1()/MADDU1() and
		// x86/ix86-32/iR5900MultDiv.cpp::recMADD*(). The accumulator is the
		// raw 64-bit value formed from LO.UL[lane*2] and HI.UL[lane*2]; the
		// writeback stores each 32-bit result half as a sign-extended 64-bit lane.
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		const size_t lo_offset = HiloLaneOffset(LO_OFFSET, upper_pipeline);
		const size_t hi_offset = HiloLaneOffset(HI_OFFSET, upper_pipeline);

		unsigned rs_host;
		unsigned rt_host;
		if (!EmitGprLowOperand(rs, HOST_TMP0, &rs_host) ||
			!EmitGprLowOperand(rt, HOST_TMP1, &rt_host))
		{
			return false;
		}

		if (signed_multiply)
		{
			if (!m_code.EmitSmull(HOST_TMP2, HOST_TMP3, rs_host, rt_host))
				return false;
		}
		else
		{
			if (!m_code.EmitUmull(HOST_TMP2, HOST_TMP3, rs_host, rt_host))
				return false;
		}

		unsigned result_low = HOST_TMP2;
		if (rd != 0)
		{
			const int low_pin = FindGprPinHost(rd);
			if (low_pin >= 0)
			{
				const unsigned low_candidate = static_cast<unsigned>(low_pin);
				if (low_candidate != HOST_TMP3)
	{
					result_low = low_candidate;
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuGprPinLowResultStoreOperands++;
#endif
	}
			}
		}

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(lo_offset)) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(hi_offset)) ||
			!m_code.EmitAddReg(result_low, HOST_TMP2, HOST_TMP0, true) ||
			!m_code.EmitAdcReg(HOST_TMP3, HOST_TMP3, HOST_TMP1))
		{
			return false;
		}

		unsigned rd_high = HOST_TMP0;
		if (rd != 0)
		{
			const int high_pin = FindGprPinHighHost(rd);
			if (high_pin >= 0)
			{
				const unsigned high_candidate = static_cast<unsigned>(high_pin);
				if (high_candidate != result_low && high_candidate != HOST_TMP3)
					rd_high = high_candidate;
			}
		}

		if (!m_code.EmitMovRegShiftImm(rd_high, result_low, VitaA32::ShiftType::ASR, 31) ||
			!m_code.EmitStrImm12(result_low, HOST_CPU_REGS, static_cast<u16>(lo_offset)) ||
			!m_code.EmitStrImm12(rd_high, HOST_CPU_REGS, static_cast<u16>(lo_offset + sizeof(u32))))
		{
			return false;
		}

		if (rd != 0 && !EmitStoreGpr64(rd, result_low, rd_high))
			return false;

		return m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP3, VitaA32::ShiftType::ASR, 31) &&
			   m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(hi_offset)) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(hi_offset + sizeof(u32)));
	}

	bool BlockCompiler::EmitDIV(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::DIV(). Cortex-A9 has no integer
		// divide, so no-divide and positive power-of-two cases are direct and
		// arbitrary division keeps the exact PCSX2 helper semantics.
		return EmitScalarDivide(op, true, false);
	}

	bool BlockCompiler::EmitDIVU(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::DIVU().
		return EmitScalarDivide(op, false, false);
	}

	bool BlockCompiler::EmitDIV1(u32 op)
	{
		// PCSX2 owners: MMI.cpp::DIV1() and x86/ix86-32/iR5900MultDiv.cpp::recDIV1().
		return EmitScalarDivide(op, true, true);
	}

	bool BlockCompiler::EmitDIVU1(u32 op)
	{
		// PCSX2 owners: MMI.cpp::DIVU1() and x86/ix86-32/iR5900MultDiv.cpp::recDIVU1().
		return EmitScalarDivide(op, false, true);
	}

	bool BlockCompiler::EmitScalarDivide(u32 op, bool signed_divide, bool upper_pipeline)
	{
		struct BranchPatch
		{
			size_t offset = static_cast<size_t>(-1);
			VitaA32::Condition condition = VitaA32::Condition::AL;
		};

		const auto emit_branch = [this](BranchPatch& patch, VitaA32::Condition condition) {
			patch.offset = m_code.EmitBranchPlaceholder(condition);
			patch.condition = condition;
			return patch.offset != static_cast<size_t>(-1);
		};

		const auto patch_branch = [this](const BranchPatch& patch, size_t target) {
			return m_code.PatchBranch(patch.offset, target, patch.condition);
		};

		const auto patch_branches = [patch_branch](const BranchPatch* branches, unsigned count, size_t target) {
			for (unsigned i = 0; i < count; i++)
			{
				if (!patch_branch(branches[i], target))
					return false;
			}
			return true;
		};

		const auto store_signed_word_as_doubleword = [this](unsigned value_reg, size_t offset) {
			return m_code.EmitMovRegShiftImm(HOST_TMP4, value_reg, VitaA32::ShiftType::ASR, 31) &&
				   m_code.EmitStrImm12(value_reg, HOST_CPU_REGS, static_cast<u16>(offset)) &&
				   m_code.EmitStrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
		};

		const size_t lo_offset = HiloLaneOffset(LO_OFFSET, upper_pipeline);
		const size_t hi_offset = HiloLaneOffset(HI_OFFSET, upper_pipeline);

		unsigned dividend_low;
		unsigned divisor_low;
		if (!EmitGprLowOperand(RS(op), HOST_TMP0, &dividend_low) ||
			!EmitGprLowOperand(RT(op), HOST_TMP1, &divisor_low))
		{
			return false;
		}

		const auto materialize_divide_operands = [this, dividend_low, divisor_low]() {
			if (dividend_low == HOST_TMP1 && divisor_low == HOST_TMP0)
			{
				return m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 0) &&
					   m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP1, VitaA32::ShiftType::LSL, 0) &&
					   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP2, VitaA32::ShiftType::LSL, 0);
			}

			if (divisor_low == HOST_TMP0 && dividend_low != HOST_TMP0)
			{
				if (!m_code.EmitMovRegShiftImm(HOST_TMP1, divisor_low, VitaA32::ShiftType::LSL, 0))
					return false;
			}

			if (dividend_low != HOST_TMP0 &&
				!m_code.EmitMovRegShiftImm(HOST_TMP0, dividend_low, VitaA32::ShiftType::LSL, 0))
			{
				return false;
			}

			if (divisor_low != HOST_TMP1 &&
				!m_code.EmitMovRegShiftImm(HOST_TMP1, divisor_low, VitaA32::ShiftType::LSL, 0))
			{
				return false;
			}

			return true;
		};

		const auto emit_unsigned_shift_subtract_divide =
			[this, emit_branch, patch_branch, patch_branches](unsigned dividend_reg, unsigned divisor_reg,
				unsigned quotient_reg, unsigned remainder_reg) {
				BranchPatch below_branch{};
				BranchPatch equal_branch{};
				BranchPatch done_branches[2]{};
				unsigned done_branch_count = 0;

				if (!m_code.EmitMovImm8(quotient_reg, 0) ||
					!m_code.EmitMovRegShiftImm(remainder_reg, dividend_reg, VitaA32::ShiftType::LSL, 0) ||
					!m_code.EmitCmpReg(dividend_reg, divisor_reg) ||
					!emit_branch(below_branch, VitaA32::Condition::CC) ||
					!emit_branch(equal_branch, VitaA32::Condition::EQ))
	{
					return false;
	}

				if (!m_code.EmitClz(quotient_reg, divisor_reg) ||
					!m_code.EmitClz(dividend_reg, dividend_reg) ||
					!m_code.EmitSubReg(quotient_reg, quotient_reg, dividend_reg) ||
					!m_code.EmitMovRegShiftReg(dividend_reg, divisor_reg,
						VitaA32::ShiftType::LSL, quotient_reg) ||
					!m_code.EmitMovImm8(divisor_reg, 1) ||
					!m_code.EmitMovRegShiftReg(divisor_reg, divisor_reg,
						VitaA32::ShiftType::LSL, quotient_reg) ||
					!m_code.EmitMovImm8(quotient_reg, 0))
	{
					return false;
	}

				const size_t loop_start = m_code.Size();
				BranchPatch skip_subtract_branch{};
				BranchPatch loop_branch{};
				if (!m_code.EmitCmpReg(remainder_reg, dividend_reg) ||
					!emit_branch(skip_subtract_branch, VitaA32::Condition::CC) ||
					!m_code.EmitSubReg(remainder_reg, remainder_reg, dividend_reg) ||
					!m_code.EmitOrrReg(quotient_reg, quotient_reg, divisor_reg) ||
					!patch_branch(skip_subtract_branch, m_code.Size()) ||
					!m_code.EmitMovRegShiftImm(divisor_reg, divisor_reg, VitaA32::ShiftType::LSR, 1, true) ||
					!m_code.EmitMovRegShiftImm(dividend_reg, dividend_reg, VitaA32::ShiftType::LSR, 1) ||
					!emit_branch(loop_branch, VitaA32::Condition::NE) ||
					!patch_branch(loop_branch, loop_start) ||
					!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
	{
					return false;
	}

				if (!patch_branch(equal_branch, m_code.Size()) ||
					!m_code.EmitMovImm8(quotient_reg, 1) ||
					!m_code.EmitMovImm8(remainder_reg, 0) ||
					!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
					!patch_branch(below_branch, m_code.Size()))
	{
					return false;
	}

				return patch_branches(done_branches, done_branch_count, m_code.Size());
			};

		BranchPatch divzero_branch{};
		BranchPatch zero_branch{};
		BranchPatch divone_branch{};
		BranchPatch negone_or_below_branch{};
		BranchPatch equal_branch{};
		BranchPatch power_of_two_branch{};
		BranchPatch fallback_branch{};
		BranchPatch done_branches[6]{};
		unsigned done_branch_count = 0;

		if (!m_code.EmitCmpImm32(divisor_low, 0) ||
			!emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
			!m_code.EmitCmpImm32(dividend_low, 0) ||
			!emit_branch(zero_branch, VitaA32::Condition::EQ) ||
			!m_code.EmitCmpImm32(divisor_low, 1) ||
			!emit_branch(divone_branch, VitaA32::Condition::EQ))
		{
			return false;
		}

		if (signed_divide)
		{
			if (!m_code.EmitCmpImm32(divisor_low, 0xffffffffu) ||
				!emit_branch(negone_or_below_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(dividend_low, divisor_low) ||
				!emit_branch(equal_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP3, divisor_low, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP2, divisor_low, HOST_TMP3) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP3) ||
				!m_code.EmitSubImm8(HOST_TMP3, HOST_TMP2, 1) ||
				!m_code.EmitAndReg(HOST_TMP2, HOST_TMP2, HOST_TMP3, true) ||
				!emit_branch(power_of_two_branch, VitaA32::Condition::EQ))
			{
				return false;
			}
		}
		else if (!m_code.EmitCmpReg(dividend_low, divisor_low) ||
				 !emit_branch(negone_or_below_branch, VitaA32::Condition::CC) ||
				 !emit_branch(equal_branch, VitaA32::Condition::EQ) ||
				 !m_code.EmitSubImm8(HOST_TMP2, divisor_low, 1) ||
				 !m_code.EmitAndReg(HOST_TMP2, divisor_low, HOST_TMP2, true) ||
				 !emit_branch(power_of_two_branch, VitaA32::Condition::EQ))
		{
			return false;
		}

		if (!emit_branch(fallback_branch, VitaA32::Condition::AL))
			return false;

		if (!patch_branch(divzero_branch, m_code.Size()) ||
			!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu))
		{
			return false;
		}

		if (signed_divide &&
			(!m_code.EmitCmpImm32(dividend_low, 0) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::LT)))
		{
			return false;
		}

		if (!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
			!store_signed_word_as_doubleword(dividend_low, hi_offset) ||
			!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(zero_branch, m_code.Size()) ||
			!m_code.EmitMovImm8(HOST_TMP2, 0) ||
			!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
			!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
			!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(divone_branch, m_code.Size()) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP2, dividend_low, VitaA32::ShiftType::LSL, 0) ||
			!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
			!m_code.EmitMovImm8(HOST_TMP2, 0) ||
			!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
			!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
		{
			return false;
		}

		if (signed_divide)
		{
			if (!patch_branch(negone_or_below_branch, m_code.Size()) ||
				!m_code.EmitRsbImm32(HOST_TMP2, dividend_low, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(power_of_two_branch, m_code.Size()) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP3, dividend_low, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, divisor_low, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP4, divisor_low, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP4, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
				!m_code.EmitAddReg(HOST_TMP3, dividend_low, HOST_TMP3) ||
				!m_code.EmitClz(HOST_TMP4, HOST_TMP4) ||
				!m_code.EmitRsbImm32(HOST_TMP2, HOST_TMP4, 31) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP4, HOST_TMP3, VitaA32::ShiftType::ASR, HOST_TMP2) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_TMP4, VitaA32::ShiftType::LSL, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP3, dividend_low, HOST_TMP3) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, divisor_low, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP4, HOST_TMP2) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP3, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}
		}
		else
		{
			if (!patch_branch(negone_or_below_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(dividend_low, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(power_of_two_branch, m_code.Size()) ||
				!m_code.EmitSubImm8(HOST_TMP2, divisor_low, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, dividend_low, HOST_TMP2) ||
				!m_code.EmitClz(HOST_TMP2, divisor_low) ||
				!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP2, 31) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP2, dividend_low, VitaA32::ShiftType::LSR, HOST_TMP4) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP3, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}
		}

		if (!patch_branch(fallback_branch, m_code.Size()) ||
			!materialize_divide_operands())
		{
			return false;
		}

		if (signed_divide)
		{
			// Cortex-A9 has no integer divide. Generate the same signed
			// trunc-toward-zero result as PCSX2's R5900OpcodeImpl.cpp/MMI.cpp
			// helpers by dividing absolute values and restoring the signs.
			if (!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP4, HOST_TMP2, HOST_TMP5) ||
				!m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP5) ||
				!m_code.EmitSubReg(HOST_TMP0, HOST_TMP0, HOST_TMP5) ||
				!m_code.EmitEorReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) ||
				!emit_unsigned_shift_subtract_divide(HOST_TMP0, HOST_TMP1, HOST_TMP2, HOST_TMP3) ||
				!m_code.EmitEorReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
				!m_code.EmitEorReg(HOST_TMP3, HOST_TMP3, HOST_TMP5) ||
				!m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_TMP5) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP3, hi_offset))
			{
				return false;
			}
		}
		else if (!emit_unsigned_shift_subtract_divide(HOST_TMP0, HOST_TMP1, HOST_TMP2, HOST_TMP3) ||
				 !store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				 !store_signed_word_as_doubleword(HOST_TMP3, hi_offset))
		{
			return false;
		}

		return patch_branches(done_branches, done_branch_count, m_code.Size());
	}

	bool BlockCompiler::EmitPLZCW(u32 op)
	{
		// PCSX2 owner: MMI.cpp::PLZCW(), which writes only RD.UL[0]/[1] with
		// the leading sign-bit count of RS.SL[0]/[1], excluding the sign bit.
		const unsigned rd = RD(op);
		if (rd == 0)
			return true;

		const unsigned rs = RS(op);
		const int cached_qreg = FindGprQCache(rs);
		if (cached_qreg >= 0)
		{
			unsigned result_qreg = 0;
			for (; result_qreg < MAX_GPR_QCACHE; result_qreg++)
			{
				if (result_qreg != static_cast<unsigned>(cached_qreg))
					break;
			}
			InvalidateGprQCacheForQreg(result_qreg);
			return m_code.EmitVclsS32Q(result_qreg, static_cast<unsigned>(cached_qreg)) &&
				   EmitMoveQWordLaneToCore(HOST_TMP0, result_qreg, 0) &&
				   EmitMoveQWordLaneToCore(HOST_TMP1, result_qreg, 1) &&
				   EmitStoreGprWord(rd, 0, HOST_TMP0) &&
				   EmitStoreGprWord(rd, 1, HOST_TMP1);
		}

		unsigned rs_low;
		unsigned rs_high;
		if (!EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high))
			return false;

		const auto emit_lane = [this, rd](unsigned word, unsigned source_reg, unsigned result_reg) {
			return m_code.EmitMovRegShiftImm(HOST_TMP2, source_reg, VitaA32::ShiftType::ASR, 31) &&
				   m_code.EmitEorReg(result_reg, source_reg, HOST_TMP2) &&
				   m_code.EmitClz(result_reg, result_reg) &&
				   m_code.EmitSubImm8(result_reg, result_reg, 1) &&
				   EmitStoreGprWord(rd, word, result_reg);
		};

		return emit_lane(0, rs_low, HOST_TMP0) &&
			   emit_lane(1, rs_high, HOST_TMP1);
	}

	bool BlockCompiler::EmitPMFHL(u32 op)
	{
		// PCSX2 owner: MMI.cpp::PMFHL(). Mode 2 saturates the signed 64-bit
		// LO/HI pair into a sign-extended signed 32-bit doubleword.
		const unsigned rd = RD(op);
		if (rd == 0)
			return true;

		const auto emit_word_vector = [this, rd](unsigned source_word) {
			constexpr unsigned NEON_RESULT = 0;
			InvalidateGprQCacheForQreg(NEON_RESULT);
			return m_code.EmitVldrSImm(NEON_RESULT * 4, HOST_CPU_REGS,
					   static_cast<u16>(LO_OFFSET + source_word * sizeof(u32))) &&
				   m_code.EmitVldrSImm(NEON_RESULT * 4 + 1, HOST_CPU_REGS,
					   static_cast<u16>(HI_OFFSET + source_word * sizeof(u32))) &&
				   m_code.EmitVldrSImm(NEON_RESULT * 4 + 2, HOST_CPU_REGS,
					   static_cast<u16>(LO_OFFSET + (source_word + 2) * sizeof(u32))) &&
				   m_code.EmitVldrSImm(NEON_RESULT * 4 + 3, HOST_CPU_REGS,
					   static_cast<u16>(HI_OFFSET + (source_word + 2) * sizeof(u32))) &&
				   EmitStoreGprQ128(rd, NEON_RESULT, HOST_TMP2);
		};
		const auto emit_halfword_vector = [this, rd]() {
			constexpr unsigned NEON_RESULT = 0;
			constexpr unsigned NEON_HI = 1;
			InvalidateGprQCacheForQreg(NEON_RESULT);
			InvalidateGprQCacheForQreg(NEON_HI);
			return EmitLoadCpuRegsQ128(LO_OFFSET, NEON_RESULT, HOST_TMP0) &&
				   EmitLoadCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1) &&
				   m_code.EmitVuzpI16Q(NEON_RESULT, NEON_HI) &&
				   m_code.EmitVtrnI32D(NEON_RESULT * 2, NEON_RESULT * 2 + 1) &&
				   EmitStoreGprQ128(rd, NEON_RESULT, HOST_TMP2);
		};
		const auto emit_saturating_halfword_vector = [this, rd]() {
			constexpr unsigned NEON_RESULT = 0;
			constexpr unsigned NEON_HI = 1;
			constexpr unsigned NEON_RESULT_LOW_D = NEON_RESULT * 2;
			constexpr unsigned NEON_RESULT_HIGH_D = NEON_RESULT_LOW_D + 1;
			InvalidateGprQCacheForQreg(NEON_RESULT);
			InvalidateGprQCacheForQreg(NEON_HI);
			return EmitLoadCpuRegsQ128(LO_OFFSET, NEON_RESULT, HOST_TMP0) &&
				   EmitLoadCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1) &&
				   m_code.EmitVqmovnS32D(NEON_RESULT_LOW_D, NEON_RESULT) &&
				   m_code.EmitVqmovnS32D(NEON_RESULT_HIGH_D, NEON_HI) &&
				   m_code.EmitVtrnI32D(NEON_RESULT_LOW_D, NEON_RESULT_HIGH_D) &&
				   EmitStoreGprQ128(rd, NEON_RESULT, HOST_TMP2);
		};
		const auto emit_saturating_word_vector = [this, rd]() {
			constexpr unsigned NEON_RESULT = 0;
			constexpr unsigned NEON_SIGN = 1;
			constexpr unsigned NEON_RESULT_LOW_D = NEON_RESULT * 2;
			constexpr unsigned NEON_RESULT_HIGH_D = NEON_RESULT_LOW_D + 1;
			constexpr unsigned NEON_SIGN_LOW_D = NEON_SIGN * 2;
			InvalidateGprQCacheForQreg(NEON_RESULT);
			InvalidateGprQCacheForQreg(NEON_SIGN);
			return m_code.EmitVldrSImm(NEON_RESULT * 4, HOST_CPU_REGS,
					   static_cast<u16>(LO_OFFSET)) &&
				   m_code.EmitVldrSImm(NEON_RESULT * 4 + 1, HOST_CPU_REGS,
					   static_cast<u16>(HI_OFFSET)) &&
				   m_code.EmitVldrSImm(NEON_RESULT * 4 + 2, HOST_CPU_REGS,
					   static_cast<u16>(LO_OFFSET + 2 * sizeof(u32))) &&
				   m_code.EmitVldrSImm(NEON_RESULT * 4 + 3, HOST_CPU_REGS,
					   static_cast<u16>(HI_OFFSET + 2 * sizeof(u32))) &&
				   m_code.EmitVqmovnS64D(NEON_RESULT_LOW_D, NEON_RESULT) &&
				   m_code.EmitVorrD(NEON_SIGN_LOW_D, NEON_RESULT_LOW_D, NEON_RESULT_LOW_D) &&
				   m_code.EmitVshrS32Q(NEON_SIGN, NEON_SIGN, 31) &&
				   m_code.EmitVtrnI32D(NEON_RESULT_LOW_D, NEON_SIGN_LOW_D) &&
				   m_code.EmitVorrD(NEON_RESULT_HIGH_D, NEON_SIGN_LOW_D, NEON_SIGN_LOW_D) &&
				   EmitStoreGprQ128(rd, NEON_RESULT, HOST_TMP2);
		};

		switch (SA(op))
		{
			case 0x00: // LW
				return emit_word_vector(0);
			case 0x01: // UW
				return emit_word_vector(1);
			case 0x02: // SLW
				return emit_saturating_word_vector();
			case 0x03: // LH
				return emit_halfword_vector();
			case 0x04: // SH
				return emit_saturating_halfword_vector();
			default:
				return true;
		}
	}

	bool BlockCompiler::EmitPMTHL(u32 op)
	{
		// PCSX2 owner: MMI.cpp::PMTHL(), which only changes LO/HI for SA == 0.
		if (SA(op) != 0)
			return true;

		const unsigned rs = RS(op);
		const auto emit_store_core_word = [this](unsigned source_reg, size_t dst_offset) {
			return m_code.EmitStrImm12(source_reg, HOST_CPU_REGS, static_cast<u16>(dst_offset));
		};

		if (rs == 0)
		{
			return m_code.EmitMovImm8(HOST_TMP0, 0) &&
				   emit_store_core_word(HOST_TMP0, LO_OFFSET) &&
				   emit_store_core_word(HOST_TMP0, HI_OFFSET) &&
				   emit_store_core_word(HOST_TMP0, LO_OFFSET + 2 * sizeof(u32)) &&
				   emit_store_core_word(HOST_TMP0, HI_OFFSET + 2 * sizeof(u32));
		}

		const int cached_qreg = FindGprQCache(rs);
		if (cached_qreg >= 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheWordHits += 4;
#endif
			const unsigned source_qreg = static_cast<unsigned>(cached_qreg);
			return EmitStoreQWordLane(source_qreg, 0, HOST_CPU_REGS,
					   static_cast<u16>(LO_OFFSET), HOST_TMP0) &&
				   EmitStoreQWordLane(source_qreg, 1, HOST_CPU_REGS,
					   static_cast<u16>(HI_OFFSET), HOST_TMP0) &&
				   EmitStoreQWordLane(source_qreg, 2, HOST_CPU_REGS,
					   static_cast<u16>(LO_OFFSET + 2 * sizeof(u32)), HOST_TMP0) &&
				   EmitStoreQWordLane(source_qreg, 3, HOST_CPU_REGS,
					   static_cast<u16>(HI_OFFSET + 2 * sizeof(u32)), HOST_TMP0);
		}

		unsigned rs_qreg = 0;
		for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
		{
			if (!IsGprQCacheQregResident(qreg))
			{
				rs_qreg = qreg;
				break;
			}
		}

		const auto emit_store_source_word = [this, rs_qreg](unsigned source_word, size_t dst_offset) {
			return EmitStoreQWordLane(rs_qreg, source_word, HOST_CPU_REGS,
				static_cast<u16>(dst_offset), HOST_TMP0);
		};

		return EmitLoadGprQ128(rs, rs_qreg, HOST_TMP0) &&
			   emit_store_source_word(0, LO_OFFSET) &&
			   emit_store_source_word(1, HI_OFFSET) &&
			   emit_store_source_word(2, LO_OFFSET + 2 * sizeof(u32)) &&
			   emit_store_source_word(3, HI_OFFSET + 2 * sizeof(u32));
	}

	bool BlockCompiler::EmitMFHI(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MFHI().
		return EmitMoveFromHiLo(op, HI_OFFSET);
	}

	bool BlockCompiler::EmitMFLO(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MFLO().
		return EmitMoveFromHiLo(op, LO_OFFSET);
	}

	bool BlockCompiler::EmitMTHI(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MTHI().
		return EmitMoveToHiLo(op, HI_OFFSET);
	}

	bool BlockCompiler::EmitMTLO(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MTLO().
		return EmitMoveToHiLo(op, LO_OFFSET);
	}

	bool BlockCompiler::EmitMFSA(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MFSA().
		const unsigned rd = RD(op);
		if (rd == 0)
			return true;

		const unsigned result_reg = SelectGprLowResultHost(rd, HOST_TMP0);
		return m_code.EmitLdrImm12(result_reg, HOST_CPU_REGS, static_cast<u16>(SA_OFFSET)) &&
			   EmitStoreGprZeroExtended32FromLow(rd, result_reg);
	}

	bool BlockCompiler::EmitMTSA(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MTSA().
		u32 value = 0;
		if (TryGetKnownGprLow(RS(op), &value))
		{
			m_sa_const_known = true;
			m_sa_const_byte_offset = static_cast<u8>(value & 0x0f);
		}
		else
		{
			ClearSaConstState();
		}

		unsigned rs_host;
		return EmitGprLowOperand(RS(op), HOST_TMP0, &rs_host) &&
			   m_code.EmitStrImm12(rs_host, HOST_CPU_REGS, static_cast<u16>(SA_OFFSET));
	}

	bool BlockCompiler::EmitMoveFromHiLo(u32 op, size_t hilo_offset)
	{
		const unsigned rd = RD(op);
		if (rd == 0)
			return true;

		const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
		const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
		return EmitLoadCpuRegsU64(hilo_offset, low_result, high_result, HOST_TMP2) &&
			   EmitStoreGpr64(rd, low_result, high_result);
	}

	bool BlockCompiler::EmitMoveToHiLo(u32 op, size_t hilo_offset)
	{
		unsigned rs_low;
		unsigned rs_high;
		return EmitGpr64ReadOperands(RS(op), HOST_TMP0, HOST_TMP1, &rs_low, &rs_high) &&
			   EmitStoreCpuRegsU64(hilo_offset, rs_low, rs_high, HOST_TMP2);
	}

	bool BlockCompiler::EmitMoveFullFromHiLo(u32 op, size_t hilo_offset)
	{
		const unsigned rd = RD(op);
		if (rd == 0)
			return true;

		unsigned value_qreg = 0;
		for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
		{
			if (!IsGprQCacheQregResident(qreg))
			{
				value_qreg = qreg;
				break;
			}
		}

		InvalidateGprQCacheForQreg(value_qreg);
		return EmitLoadCpuRegsQ128(hilo_offset, value_qreg, HOST_TMP0) &&
			   EmitStoreGprQ128(rd, value_qreg, HOST_TMP1);
	}

	bool BlockCompiler::EmitMoveFullToHiLo(u32 op, size_t hilo_offset)
	{
		const unsigned rs = RS(op);
		const int cached_qreg = FindGprQCache(rs);
		if (cached_qreg >= 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheDirectHiLoStores++;
#endif
			return EmitStoreCpuRegsQ128(hilo_offset, static_cast<unsigned>(cached_qreg), HOST_TMP1);
		}

		unsigned value_qreg = 0;
		for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
		{
			if (!IsGprQCacheQregResident(qreg))
			{
				value_qreg = qreg;
				break;
			}
		}

		if (!EmitLoadGprQ128(rs, value_qreg, HOST_TMP0))
			return false;

		return EmitStoreCpuRegsQ128(hilo_offset, value_qreg, HOST_TMP1);
	}

	bool BlockCompiler::EmitMMI(u32 op)
	{
		switch (op & 0x3f)
		{
			case 0x00: // MADD, owned by MMI.cpp::MADD().
				return EmitMADD(op);
			case 0x01: // MADDU, owned by MMI.cpp::MADDU().
				return EmitMADDU(op);
			case 0x04: // PLZCW, owned by MMI.cpp::PLZCW().
				return EmitPLZCW(op);
			case 0x08: // MMI0 class, owned by R5900OpcodeTables.cpp::Class_MMI0().
				return EmitMMI0(op);
			case 0x09: // MMI2 class, owned by R5900OpcodeTables.cpp::Class_MMI2().
				return EmitMMI2(op);
			case 0x28: // MMI1 class, owned by R5900OpcodeTables.cpp::Class_MMI1().
				return EmitMMI1(op);
			case 0x29: // MMI3 class, owned by R5900OpcodeTables.cpp::Class_MMI3().
				return EmitMMI3(op);
			case 0x10: // MFHI1, owned by MMI.cpp::MFHI1().
				return EmitMFHI1(op);
			case 0x11: // MTHI1, owned by MMI.cpp::MTHI1().
				return EmitMTHI1(op);
			case 0x12: // MFLO1, owned by MMI.cpp::MFLO1().
				return EmitMFLO1(op);
			case 0x13: // MTLO1, owned by MMI.cpp::MTLO1().
				return EmitMTLO1(op);
			case 0x18: // MULT1, owned by MMI.cpp::MULT1().
				return EmitMULT1(op);
			case 0x19: // MULTU1, owned by MMI.cpp::MULTU1().
				return EmitMULTU1(op);
			case 0x1a: // DIV1, owned by MMI.cpp::DIV1().
				return EmitDIV1(op);
			case 0x1b: // DIVU1, owned by MMI.cpp::DIVU1().
				return EmitDIVU1(op);
			case 0x20: // MADD1, owned by MMI.cpp::MADD1().
				return EmitMADD1(op);
			case 0x21: // MADDU1, owned by MMI.cpp::MADDU1().
				return EmitMADDU1(op);
			case 0x30: // PMFHL, owned by MMI.cpp::PMFHL().
				return EmitPMFHL(op);
			case 0x31: // PMTHL, owned by MMI.cpp::PMTHL().
				return EmitPMTHL(op);
			case 0x34: // PSLLH, owned by MMI.cpp::PSLLH().
				return EmitPSLLH(op);
			case 0x36: // PSRLH, owned by MMI.cpp::PSRLH().
				return EmitPSRLH(op);
			case 0x37: // PSRAH, owned by MMI.cpp::PSRAH().
				return EmitPSRAH(op);
			case 0x3c: // PSLLW, owned by MMI.cpp::PSLLW().
				return EmitPSLLW(op);
			case 0x3e: // PSRLW, owned by MMI.cpp::PSRLW().
				return EmitPSRLW(op);
			case 0x3f: // PSRAW, owned by MMI.cpp::PSRAW().
				return EmitPSRAW(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitMMI0(u32 op)
	{
		switch ((op >> 6) & 0x1f)
		{
			case 0x00: // PADDW, owned by MMI.cpp::PADDW().
				return EmitPADDW(op);
			case 0x01: // PSUBW, owned by MMI.cpp::PSUBW().
				return EmitPSUBW(op);
			case 0x02: // PCGTW, owned by MMI.cpp::PCGTW().
				return EmitPCGTW(op);
			case 0x03: // PMAXW, owned by MMI.cpp::PMAXW().
				return EmitPMAXW(op);
			case 0x04: // PADDH, owned by MMI.cpp::PADDH().
				return EmitPADDH(op);
			case 0x05: // PSUBH, owned by MMI.cpp::PSUBH().
				return EmitPSUBH(op);
			case 0x06: // PCGTH, owned by MMI.cpp::PCGTH().
				return EmitPCGTH(op);
			case 0x07: // PMAXH, owned by MMI.cpp::PMAXH().
				return EmitPMAXH(op);
			case 0x08: // PADDB, owned by MMI.cpp::PADDB().
				return EmitPADDB(op);
			case 0x09: // PSUBB, owned by MMI.cpp::PSUBB().
				return EmitPSUBB(op);
			case 0x0a: // PCGTB, owned by MMI.cpp::PCGTB().
				return EmitPCGTB(op);
			case 0x10: // PADDSW, owned by MMI.cpp::PADDSW().
				return EmitPADDSW(op);
			case 0x11: // PSUBSW, owned by MMI.cpp::PSUBSW().
				return EmitPSUBSW(op);
			case 0x12: // PEXTLW, owned by MMI.cpp::PEXTLW().
				return EmitPEXTLW(op);
			case 0x13: // PPACW, owned by MMI.cpp::PPACW().
				return EmitPPACW(op);
			case 0x14: // PADDSH, owned by MMI.cpp::PADDSH().
				return EmitPADDSH(op);
			case 0x15: // PSUBSH, owned by MMI.cpp::PSUBSH().
				return EmitPSUBSH(op);
			case 0x16: // PEXTLH, owned by MMI.cpp::PEXTLH().
				return EmitPEXTLH(op);
			case 0x17: // PPACH, owned by MMI.cpp::PPACH().
				return EmitPPACH(op);
			case 0x18: // PADDSB, owned by MMI.cpp::PADDSB().
				return EmitPADDSB(op);
			case 0x19: // PSUBSB, owned by MMI.cpp::PSUBSB().
				return EmitPSUBSB(op);
			case 0x1a: // PEXTLB, owned by MMI.cpp::PEXTLB().
				return EmitPEXTLB(op);
			case 0x1b: // PPACB, owned by MMI.cpp::PPACB().
				return EmitPPACB(op);
			case 0x1e: // PEXT5, owned by MMI.cpp::PEXT5().
				return EmitPEXT5(op);
			case 0x1f: // PPAC5, owned by MMI.cpp::PPAC5().
				return EmitPPAC5(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitMMI1(u32 op)
	{
		switch ((op >> 6) & 0x1f)
		{
			case 0x01: // PABSW, owned by MMI.cpp::PABSW().
				return EmitPABSW(op);
			case 0x02: // PCEQW, owned by MMI.cpp::PCEQW().
				return EmitPCEQW(op);
			case 0x03: // PMINW, owned by MMI.cpp::PMINW().
				return EmitPMINW(op);
			case 0x04: // PADSBH, owned by MMI.cpp::PADSBH().
				return EmitPADSBH(op);
			case 0x05: // PABSH, owned by MMI.cpp::PABSH().
				return EmitPABSH(op);
			case 0x06: // PCEQH, owned by MMI.cpp::PCEQH().
				return EmitPCEQH(op);
			case 0x07: // PMINH, owned by MMI.cpp::PMINH().
				return EmitPMINH(op);
			case 0x0a: // PCEQB, owned by MMI.cpp::PCEQB().
				return EmitPCEQB(op);
			case 0x10: // PADDUW, owned by MMI.cpp::PADDUW().
				return EmitPADDUW(op);
			case 0x11: // PSUBUW, owned by MMI.cpp::PSUBUW().
				return EmitPSUBUW(op);
			case 0x12: // PEXTUW, owned by MMI.cpp::PEXTUW().
				return EmitPEXTUW(op);
			case 0x14: // PADDUH, owned by MMI.cpp::PADDUH().
				return EmitPADDUH(op);
			case 0x15: // PSUBUH, owned by MMI.cpp::PSUBUH().
				return EmitPSUBUH(op);
			case 0x16: // PEXTUH, owned by MMI.cpp::PEXTUH().
				return EmitPEXTUH(op);
			case 0x18: // PADDUB, owned by MMI.cpp::PADDUB().
				return EmitPADDUB(op);
			case 0x19: // PSUBUB, owned by MMI.cpp::PSUBUB().
				return EmitPSUBUB(op);
			case 0x1a: // PEXTUB, owned by MMI.cpp::PEXTUB().
				return EmitPEXTUB(op);
			case 0x1b: // QFSRV, owned by MMI.cpp::QFSRV().
				return EmitQFSRV(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitMMI2(u32 op)
	{
		switch ((op >> 6) & 0x1f)
		{
			case 0x00: // PMADDW, owned by MMI.cpp::PMADDW().
				return EmitPMADDW(op);
			case 0x02: // PSLLVW, owned by MMI.cpp::PSLLVW().
				return EmitPSLLVW(op);
			case 0x03: // PSRLVW, owned by MMI.cpp::PSRLVW().
				return EmitPSRLVW(op);
			case 0x04: // PMSUBW, owned by MMI.cpp::PMSUBW().
				return EmitPMSUBW(op);
			case 0x08: // PMFHI, owned by MMI.cpp::PMFHI().
				return EmitPMFHI(op);
			case 0x09: // PMFLO, owned by MMI.cpp::PMFLO().
				return EmitPMFLO(op);
			case 0x0a: // PINTH, owned by MMI.cpp::PINTH().
				return EmitPINTH(op);
			case 0x0c: // PMULTW, owned by MMI.cpp::PMULTW().
				return EmitPMULTW(op);
			case 0x0d: // PDIVW, owned by MMI.cpp::PDIVW().
				return EmitPDIVW(op);
			case 0x0e: // PCPYLD, owned by MMI.cpp::PCPYLD().
				return EmitPCPYLD(op);
			case 0x10: // PMADDH, owned by MMI.cpp::PMADDH().
				return EmitPMADDH(op);
			case 0x11: // PHMADH, owned by MMI.cpp::PHMADH().
				return EmitPHMADH(op);
			case 0x12: // PAND, owned by MMI.cpp::PAND().
				return EmitPAND(op);
			case 0x13: // PXOR, owned by MMI.cpp::PXOR().
				return EmitPXOR(op);
			case 0x14: // PMSUBH, owned by MMI.cpp::PMSUBH().
				return EmitPMSUBH(op);
			case 0x15: // PHMSBH, owned by MMI.cpp::PHMSBH().
				return EmitPHMSBH(op);
			case 0x1a: // PEXEH, owned by MMI.cpp::PEXEH().
				return EmitPEXEH(op);
			case 0x1b: // PREVH, owned by MMI.cpp::PREVH().
				return EmitPREVH(op);
			case 0x1c: // PMULTH, owned by MMI.cpp::PMULTH().
				return EmitPMULTH(op);
			case 0x1d: // PDIVBW, owned by MMI.cpp::PDIVBW().
				return EmitPDIVBW(op);
			case 0x1e: // PEXEW, owned by MMI.cpp::PEXEW().
				return EmitPEXEW(op);
			case 0x1f: // PROT3W, owned by MMI.cpp::PROT3W().
				return EmitPROT3W(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitMMI3(u32 op)
	{
		switch ((op >> 6) & 0x1f)
		{
			case 0x00: // PMADDUW, owned by MMI.cpp::PMADDUW().
				return EmitPMADDUW(op);
			case 0x03: // PSRAVW, owned by MMI.cpp::PSRAVW().
				return EmitPSRAVW(op);
			case 0x08: // PMTHI, owned by MMI.cpp::PMTHI().
				return EmitPMTHI(op);
			case 0x09: // PMTLO, owned by MMI.cpp::PMTLO().
				return EmitPMTLO(op);
			case 0x0a: // PINTEH, owned by MMI.cpp::PINTEH().
				return EmitPINTEH(op);
			case 0x0c: // PMULTUW, owned by MMI.cpp::PMULTUW().
				return EmitPMULTUW(op);
			case 0x0d: // PDIVUW, owned by MMI.cpp::PDIVUW().
				return EmitPDIVUW(op);
			case 0x0e: // PCPYUD, owned by MMI.cpp::PCPYUD().
				return EmitPCPYUD(op);
			case 0x12: // POR, owned by MMI.cpp::POR().
				return EmitPOR(op);
			case 0x13: // PNOR, owned by MMI.cpp::PNOR().
				return EmitPNOR(op);
			case 0x1a: // PEXCH, owned by MMI.cpp::PEXCH().
				return EmitPEXCH(op);
			case 0x1b: // PCPYH, owned by MMI.cpp::PCPYH().
				return EmitPCPYH(op);
			case 0x1e: // PEXCW, owned by MMI.cpp::PEXCW().
				return EmitPEXCW(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitPMADDW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMADDW(); the lower lane has the PS2 high-word
		// adjustment documented in the interpreter.
		return EmitPackedSignedWordMultiplyAccumulate(op, false);
	}

	bool BlockCompiler::EmitPMSUBW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMSUBW(); x86/iMMI.cpp::recPMSUBW() uses the same
		// signed product and LO/HI accumulator lanes.
		return EmitPackedSignedWordMultiplyAccumulate(op, true);
	}

	bool BlockCompiler::EmitPMADDH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMADDH(), which adds eight signed 16x16 products
		// into the packed LO/HI word accumulators.
		return EmitPackedHalfwordMultiplyAccumulate(op, false);
	}

	bool BlockCompiler::EmitPMSUBH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMSUBH(), which subtracts eight signed 16x16
		// products from the packed LO/HI word accumulators.
		return EmitPackedHalfwordMultiplyAccumulate(op, true);
	}

	bool BlockCompiler::EmitPHMADH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PHMADH(), which writes pairwise signed 16x16
		// sums into even LO/HI words and the second pair product into odd words.
		return EmitPackedHalfwordPairMultiply(op, false);
	}

	bool BlockCompiler::EmitPHMSBH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PHMSBH(), which writes pairwise signed 16x16
		// differences and the undocumented bitwise-not second product lanes.
		return EmitPackedHalfwordPairMultiply(op, true);
	}

	bool BlockCompiler::EmitPMULTW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMULTW(), which multiplies RS.SL[0]/[2] by
		// RT.SL[0]/[2], writes the raw 64-bit products to RD, and writes the
		// sign-extended low/high 32-bit halves into LO/HI.
		return EmitPackedWordMultiply(op, true);
	}

	bool BlockCompiler::EmitPMULTUW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PMULTUW(), which multiplies RS.UL[0]/[2] by
		// RT.UL[0]/[2], writes unsigned 64-bit products to RD, and still
		// sign-extends the low/high 32-bit halves into LO/HI.
		return EmitPackedWordMultiply(op, false);
	}

	bool BlockCompiler::EmitPMULTH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMULTH(), which multiplies all eight signed
		// halfword lanes into LO/HI words and exposes lanes 0/2/4/6 through RD.
		return EmitPackedHalfwordMultiply(op);
	}

	bool BlockCompiler::EmitPMADDUW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PMADDUW(), which adds each unsigned 32x32
		// product to the raw 64-bit LO.UL[0/2] + HI.UL[0/2] accumulator.
		return EmitPackedUnsignedWordMultiplyAdd(op);
	}

	bool BlockCompiler::EmitPDIVW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PDIVW(). Cortex-A9 has no integer divide, so the
		// no-divide edge cases are emitted directly and true division keeps the
		// exact PCSX2 helper semantics.
		return EmitPackedWordDivide(op, true);
	}

	bool BlockCompiler::EmitPDIVUW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PDIVUW(). Cortex-A9 has no integer divide, so the
		// no-divide edge cases are emitted directly and true division keeps the
		// exact PCSX2 helper semantics.
		return EmitPackedWordDivide(op, false);
	}

	bool BlockCompiler::EmitPDIVBW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PDIVBW(), which divides all four signed RS words
		// by RT.SS[0] and writes all four 32-bit LO/HI words directly. Divisor
		// 0, 1, -1, power-of-two, and arbitrary divisors are all emitted as
		// A32/NEON because Cortex-A9 has no integer divide instruction.
		return EmitPackedWordByHalfwordDivide(op);
	}

	bool BlockCompiler::EmitPackedWordMultiply(u32 op, bool signed_multiply)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);

		constexpr unsigned NEON_RS = 0;
		constexpr unsigned NEON_RT = 1;
		constexpr unsigned NEON_PRODUCT = 2;
		constexpr unsigned NEON_SIGN = 3;

		if (rs == 0 || rt == 0)
		{
			unsigned zero_qreg = NEON_RS;
			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!IsGprQCacheQregResident(qreg))
				{
					zero_qreg = qreg;
					break;
				}
			}
			InvalidateGprQCacheForQreg(zero_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedWordMultiplyVectorOps++;
#endif

			// PCSX2 x86/iMMI.cpp::recPMULTW()/recPMULTUW() skip source reads and
			// zero RD/LO/HI when either multiplicand is architectural $zero.
			return m_code.EmitVeorQ(zero_qreg, zero_qreg, zero_qreg) &&
			       EmitStoreCpuRegsQ128(LO_OFFSET, zero_qreg, HOST_TMP0) &&
			       EmitStoreCpuRegsQ128(HI_OFFSET, zero_qreg, HOST_TMP1) &&
			       EmitStoreGprQ128(rd, zero_qreg, HOST_TMP2);
		}

		const int cached_rs_qreg = FindGprQCache(rs);
		const int cached_rt_qreg = FindGprQCache(rt);
		const auto is_source_qreg = [](int qreg) {
			return qreg >= 0 &&
			       qreg != static_cast<int>(NEON_PRODUCT) &&
			       qreg != static_cast<int>(NEON_SIGN);
		};
		bool used_qregs[MAX_GPR_QCACHE]{};
		used_qregs[NEON_PRODUCT] = true;
		used_qregs[NEON_SIGN] = true;
		const auto reserve_qreg = [&](unsigned qreg) {
			if (qreg < MAX_GPR_QCACHE)
				used_qregs[qreg] = true;
		};
		const auto alloc_source_qreg = [&](int cached_qreg) {
			if (is_source_qreg(cached_qreg) &&
				!used_qregs[static_cast<unsigned>(cached_qreg)] &&
				!GprQCacheQregHasFutureQwordReadBeforeWrite(
					static_cast<unsigned>(cached_qreg)))
			{
				const unsigned qreg = static_cast<unsigned>(cached_qreg);
				used_qregs[qreg] = true;
				return qreg;
			}

			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
				{
					used_qregs[qreg] = true;
					return qreg;
				}
			}

			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg] &&
					!GprQCacheQregHasFutureQwordReadBeforeWrite(qreg))
				{
					used_qregs[qreg] = true;
					return qreg;
				}
			}

			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg])
				{
					used_qregs[qreg] = true;
					return qreg;
				}
			}

			return MAX_GPR_QCACHE;
		};

		unsigned rs_qreg = alloc_source_qreg(cached_rs_qreg);
		unsigned rt_qreg = MAX_GPR_QCACHE;

		const bool rt_reuses_rs_qreg = rs == rt;
		if (rt_reuses_rs_qreg)
		{
			rt_qreg = rs_qreg;
			reserve_qreg(rt_qreg);
		}
		else
		{
			rt_qreg = alloc_source_qreg(cached_rt_qreg);
		}
		if (rs_qreg >= MAX_GPR_QCACHE || rt_qreg >= MAX_GPR_QCACHE)
			return false;
		if (!(ShouldLoadGprQ128SingleUseEntry(rs) ?
				  EmitLoadGprQ128SingleUseEntry(rs, rs_qreg, HOST_TMP0) :
				  EmitLoadGprQ128(rs, rs_qreg, HOST_TMP0)) ||
			(!rt_reuses_rs_qreg &&
			 !(ShouldLoadGprQ128SingleUseEntry(rt) ?
				   EmitLoadGprQ128SingleUseEntry(rt, rt_qreg, HOST_TMP1) :
				   EmitLoadGprQ128(rt, rt_qreg, HOST_TMP1))))
		{
			return false;
		}

		InvalidateGprQCacheForQreg(rs_qreg);
		InvalidateGprQCacheForQreg(rt_qreg);
		InvalidateGprQCacheForQreg(NEON_PRODUCT);
		InvalidateGprQCacheForQreg(NEON_SIGN);
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuMmiPackedWordMultiplyVectorOps++;
		if (is_source_qreg(cached_rs_qreg) || is_source_qreg(cached_rt_qreg))
			g_qemuGprQCacheDirectWordMultiplyOps++;
		if (rt_reuses_rs_qreg)
			g_qemuGprQCacheSameSourceQregReuses++;
#endif

		// PCSX2 owners: MMI.cpp::_PMULTW()/_PMULTUW() and
		// x86/iMMI.cpp::recPMULTW()/recPMULTUW(). Active words 0/2 are
		// multiplied into raw 64-bit products, then LO/HI receive sign-extended
		// low/high product words. VTRN.32 on each source D pair forms the active
		// [word0, word2] vector without the copy+VUZP sequence.
		if (!m_code.EmitVtrnI32D(rs_qreg * 2, rs_qreg * 2 + 1) ||
			(!rt_reuses_rs_qreg &&
				!m_code.EmitVtrnI32D(rt_qreg * 2, rt_qreg * 2 + 1)))
		{
			return false;
		}

		const bool multiply_ok = signed_multiply ?
		                             m_code.EmitVmullS32Q(NEON_PRODUCT, rs_qreg * 2, rt_qreg * 2) :
		                             m_code.EmitVmullU32Q(NEON_PRODUCT, rs_qreg * 2, rt_qreg * 2);
		if (!multiply_ok)
			return false;

		if (rd == 0)
		{
			// PCSX2 MMI.cpp::PMULTW()/PMULTUW() still update LO/HI when RD is
			// $zero. Split the product in place instead of preserving it for RD.
			return m_code.EmitVextI8Q(rs_qreg, NEON_PRODUCT, NEON_PRODUCT, 4) &&
			       m_code.EmitVshrS32Q(NEON_SIGN, NEON_PRODUCT, 31) &&
			       m_code.EmitVtrnI32Q(NEON_PRODUCT, NEON_SIGN) &&
			       EmitStoreCpuRegsQ128(LO_OFFSET, NEON_PRODUCT, HOST_TMP0) &&
			       m_code.EmitVshrS32Q(NEON_SIGN, rs_qreg, 31) &&
			       m_code.EmitVtrnI32Q(rs_qreg, NEON_SIGN) &&
			       EmitStoreCpuRegsQ128(HI_OFFSET, rs_qreg, HOST_TMP1);
		}

		if (!m_code.EmitVorrQ(rs_qreg, NEON_PRODUCT, NEON_PRODUCT) ||
			!m_code.EmitVshrS32Q(NEON_SIGN, rs_qreg, 31) ||
			!m_code.EmitVtrnI32Q(rs_qreg, NEON_SIGN) ||
			!EmitStoreCpuRegsQ128(LO_OFFSET, rs_qreg, HOST_TMP0) ||
			!m_code.EmitVextI8Q(rs_qreg, NEON_PRODUCT, NEON_PRODUCT, 4) ||
			!m_code.EmitVshrS32Q(NEON_SIGN, rs_qreg, 31) ||
			!m_code.EmitVtrnI32Q(rs_qreg, NEON_SIGN) ||
			!EmitStoreCpuRegsQ128(HI_OFFSET, rs_qreg, HOST_TMP1))
		{
			return false;
		}

		return EmitStoreGprQ128(rd, NEON_PRODUCT, HOST_TMP2);
	}

	bool BlockCompiler::EmitPackedWordDivide(u32 op, bool signed_divide)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		struct BranchPatch
		{
			size_t offset = static_cast<size_t>(-1);
			VitaA32::Condition condition = VitaA32::Condition::AL;
		};

			const auto can_emit_single_mov_imm32 = [](u32 value) {
				return value <= 0xffu || (~value) <= 0xffu ||
				       (value != 0 && (value & (value - 1)) == 0);
			};

			const auto load_word = [this, can_emit_single_mov_imm32](
									   unsigned guest_reg, unsigned word,
									   unsigned host_reg) {
				u32 known_low = 0;
				if (word == 0 && guest_reg != 0 && FindGprPinHost(guest_reg) < 0 &&
					TryGetKnownGprLow(guest_reg, &known_low) &&
					can_emit_single_mov_imm32(known_low))
				{
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuMmiPackedWordDivideKnownWordLoads++;
#endif
					return m_code.EmitMovImm32(host_reg, known_low);
				}

				return EmitLoadGprWord(guest_reg, word, host_reg);
			};

		const auto emit_branch = [this](BranchPatch& patch,
									 VitaA32::Condition condition) {
			patch.offset = m_code.EmitBranchPlaceholder(condition);
			patch.condition = condition;
			return patch.offset != static_cast<size_t>(-1);
		};

		const auto patch_branch = [this](const BranchPatch& patch,
									  size_t target) {
			return m_code.PatchBranch(patch.offset, target, patch.condition);
		};

		const auto patch_branches =
			[patch_branch](const BranchPatch* branches, unsigned count,
				size_t target) {
				for (unsigned i = 0; i < count; i++)
				{
					if (!patch_branch(branches[i], target))
						return false;
				}
				return true;
			};

			const auto store_signed_word_as_doubleword = [this](
															 unsigned value_reg,
															 size_t offset,
															 unsigned sign_reg = HOST_TMP4,
															 unsigned address_scratch = HOST_TMP5) {
				return m_code.EmitMovRegShiftImm(sign_reg, value_reg,
						   VitaA32::ShiftType::ASR, 31) &&
				       EmitStoreCpuRegsU64(offset, value_reg, sign_reg,
						   address_scratch);
			};
			const auto store_zero_extended_word_as_doubleword =
				[this](unsigned value_reg, unsigned zero_reg, size_t offset,
					unsigned address_scratch = HOST_TMP5) {
					return EmitStoreCpuRegsU64(offset, value_reg, zero_reg,
						address_scratch);
				};

		const auto emit_unsigned_shift_subtract_divide =
			[&](unsigned dividend_reg, unsigned divisor_reg,
				unsigned quotient_reg, unsigned remainder_reg) {
				BranchPatch below_branch{};
				BranchPatch equal_branch{};
				BranchPatch done_branches[2]{};
				unsigned done_branch_count = 0;

				if (!m_code.EmitMovImm8(quotient_reg, 0) ||
					!m_code.EmitMovRegShiftImm(remainder_reg, dividend_reg,
						VitaA32::ShiftType::LSL, 0) ||
					!m_code.EmitCmpReg(dividend_reg, divisor_reg) ||
					!emit_branch(below_branch, VitaA32::Condition::CC) ||
					!emit_branch(equal_branch, VitaA32::Condition::EQ))
				{
					return false;
				}

				if (!m_code.EmitClz(quotient_reg, divisor_reg) ||
					!m_code.EmitClz(dividend_reg, dividend_reg) ||
					!m_code.EmitSubReg(quotient_reg, quotient_reg, dividend_reg) ||
					!m_code.EmitMovRegShiftReg(dividend_reg, divisor_reg,
						VitaA32::ShiftType::LSL, quotient_reg) ||
					!m_code.EmitMovImm8(divisor_reg, 1) ||
					!m_code.EmitMovRegShiftReg(divisor_reg, divisor_reg,
						VitaA32::ShiftType::LSL, quotient_reg) ||
					!m_code.EmitMovImm8(quotient_reg, 0))
				{
					return false;
				}

				const size_t loop_start = m_code.Size();
				BranchPatch skip_subtract_branch{};
				BranchPatch loop_branch{};
				if (!m_code.EmitCmpReg(remainder_reg, dividend_reg) ||
					!emit_branch(skip_subtract_branch, VitaA32::Condition::CC) ||
					!m_code.EmitSubReg(remainder_reg, remainder_reg, dividend_reg) ||
					!m_code.EmitOrrReg(quotient_reg, quotient_reg, divisor_reg) ||
					!patch_branch(skip_subtract_branch, m_code.Size()) ||
					!m_code.EmitMovRegShiftImm(divisor_reg, divisor_reg,
						VitaA32::ShiftType::LSR, 1, true) ||
					!m_code.EmitMovRegShiftImm(dividend_reg, dividend_reg,
						VitaA32::ShiftType::LSR, 1) ||
					!emit_branch(loop_branch, VitaA32::Condition::NE) ||
					!patch_branch(loop_branch, loop_start) ||
					!emit_branch(done_branches[done_branch_count++],
						VitaA32::Condition::AL))
				{
					return false;
				}

				if (!patch_branch(equal_branch, m_code.Size()) ||
					!m_code.EmitMovImm8(quotient_reg, 1) ||
					!m_code.EmitMovImm8(remainder_reg, 0) ||
					!emit_branch(done_branches[done_branch_count++],
						VitaA32::Condition::AL) ||
					!patch_branch(below_branch, m_code.Size()))
				{
					return false;
				}

				return patch_branches(done_branches, done_branch_count,
					m_code.Size());
			};

		const auto emit_unsigned_general_lane =
			[&](unsigned source_word, unsigned dest_lane, bool operands_preloaded) {
				const size_t lo_offset = LO_OFFSET + dest_lane * sizeof(u64);
				const size_t hi_offset = HI_OFFSET + dest_lane * sizeof(u64);
				BranchPatch divzero_branch{};
				BranchPatch done_branch{};

				if ((!operands_preloaded &&
						(!load_word(rs, source_word, HOST_TMP0) ||
							!load_word(rt, source_word, HOST_TMP1))) ||
					!m_code.EmitCmpImm32(HOST_TMP1, 0) ||
					!emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
					!emit_unsigned_shift_subtract_divide(HOST_TMP0, HOST_TMP1,
						HOST_TMP2, HOST_TMP3) ||
					!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
					!store_signed_word_as_doubleword(HOST_TMP3, hi_offset) ||
					!emit_branch(done_branch, VitaA32::Condition::AL))
				{
					return false;
				}

				if (!patch_branch(divzero_branch, m_code.Size()) ||
					!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) ||
					!store_signed_word_as_doubleword(HOST_TMP2, lo_offset,
						HOST_TMP3) ||
					!store_signed_word_as_doubleword(HOST_TMP0, hi_offset,
						HOST_TMP1))
				{
					return false;
				}

				return patch_branch(done_branch, m_code.Size());
			};

		const auto emit_signed_general_lane =
			[&](unsigned source_word, unsigned dest_lane, bool operands_preloaded) {
				const size_t lo_offset = LO_OFFSET + dest_lane * sizeof(u64);
				const size_t hi_offset = HI_OFFSET + dest_lane * sizeof(u64);
				BranchPatch divzero_branch{};
				BranchPatch done_branch{};

				if ((!operands_preloaded &&
						(!load_word(rs, source_word, HOST_TMP0) ||
							!load_word(rt, source_word, HOST_TMP1))) ||
					!m_code.EmitCmpImm32(HOST_TMP1, 0) ||
					!emit_branch(divzero_branch, VitaA32::Condition::EQ))
				{
					return false;
				}

				// PCSX2 MMI.cpp::_PDIVW() truncates toward zero. Cortex-A9
				// has no SDIV, so divide absolute magnitudes and restore the
				// quotient/remainder signs in generated A32.
				if (!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0,
						VitaA32::ShiftType::ASR, 31) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1,
						VitaA32::ShiftType::ASR, 31) ||
					!m_code.EmitEorReg(HOST_TMP4, HOST_TMP2, HOST_TMP5) ||
					!m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP5) ||
					!m_code.EmitSubReg(HOST_TMP0, HOST_TMP0, HOST_TMP5) ||
					!m_code.EmitEorReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) ||
					!m_code.EmitSubReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) ||
					!emit_unsigned_shift_subtract_divide(HOST_TMP0, HOST_TMP1,
						HOST_TMP2, HOST_TMP3) ||
					!m_code.EmitEorReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
					!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
					!m_code.EmitEorReg(HOST_TMP3, HOST_TMP3, HOST_TMP5) ||
					!m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_TMP5) ||
					!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
					!store_signed_word_as_doubleword(HOST_TMP3, hi_offset) ||
					!emit_branch(done_branch, VitaA32::Condition::AL))
				{
					return false;
				}

				if (!patch_branch(divzero_branch, m_code.Size()) ||
					!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) ||
					!m_code.EmitCmpImm32(HOST_TMP0, 0) ||
					!m_code.EmitMovImm8(HOST_TMP2, 1,
						VitaA32::Condition::LT) ||
					!store_signed_word_as_doubleword(HOST_TMP2, lo_offset,
						HOST_TMP3) ||
					!store_signed_word_as_doubleword(HOST_TMP0, hi_offset,
						HOST_TMP1))
				{
					return false;
				}

				return patch_branch(done_branch, m_code.Size());
			};

		BranchPatch fallback_branches[2]{};
		unsigned fallback_branch_count = 0;
		const auto emit_fallback = [&]() {
			if (fallback_branch_count >= 2)
				return false;
			return emit_branch(fallback_branches[fallback_branch_count++],
				VitaA32::Condition::AL);
		};

		const auto emit_signed_fast_guard = [&](unsigned dividend_reg,
												unsigned divisor_reg) {
			BranchPatch ok_branches[6]{};
			unsigned ok_branch_count = 0;
			if (!m_code.EmitCmpImm32(divisor_reg, 0) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!m_code.EmitCmpImm32(dividend_reg, 0) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!EmitCmpImm32OrReg(divisor_reg, 1, HOST_TMP4) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!EmitCmpImm32OrReg(divisor_reg, 0xffffffffu, HOST_TMP4) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(dividend_reg, divisor_reg) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP4, divisor_reg,
					VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP5, divisor_reg, HOST_TMP4) ||
				!m_code.EmitSubReg(HOST_TMP5, HOST_TMP5, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP4, HOST_TMP5, 1) ||
				!m_code.EmitAndReg(HOST_TMP4, HOST_TMP5, HOST_TMP4, true) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!emit_fallback())
			{
				return false;
			}

			return patch_branches(ok_branches, ok_branch_count, m_code.Size());
		};

		const auto emit_unsigned_fast_guard = [&](unsigned dividend_reg,
												  unsigned divisor_reg) {
			BranchPatch ok_branches[5]{};
			unsigned ok_branch_count = 0;
			if (!m_code.EmitCmpImm32(divisor_reg, 0) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!m_code.EmitCmpImm32(dividend_reg, 0) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!EmitCmpImm32OrReg(divisor_reg, 1, HOST_TMP4) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(dividend_reg, divisor_reg) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::LS) ||
				!m_code.EmitSubImm8(HOST_TMP4, divisor_reg, 1) ||
				!m_code.EmitAndReg(HOST_TMP4, divisor_reg, HOST_TMP4, true) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!emit_fallback())
			{
				return false;
			}

			return patch_branches(ok_branches, ok_branch_count, m_code.Size());
		};

		const auto emit_signed_direct_lane = [&](unsigned source_word,
												 unsigned dest_lane,
												 bool operands_preloaded) {
			const size_t lo_offset = LO_OFFSET + dest_lane * sizeof(u64);
			const size_t hi_offset = HI_OFFSET + dest_lane * sizeof(u64);
			BranchPatch divzero_branch{};
			BranchPatch zero_branch{};
			BranchPatch divone_branch{};
			BranchPatch negone_branch{};
			BranchPatch equal_branch{};
			BranchPatch done_branches[6]{};
			unsigned done_branch_count = 0;

			if ((!operands_preloaded &&
					(!load_word(rs, source_word, HOST_TMP0) ||
						!load_word(rt, source_word, HOST_TMP1))) ||
				!m_code.EmitCmpImm32(HOST_TMP1, 0) ||
				!emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpImm32(HOST_TMP0, 0) ||
				!emit_branch(zero_branch, VitaA32::Condition::EQ) ||
				!EmitCmpImm32OrReg(HOST_TMP1, 1, HOST_TMP3) ||
				!emit_branch(divone_branch, VitaA32::Condition::EQ) ||
				!EmitCmpImm32OrReg(HOST_TMP1, 0xffffffffu, HOST_TMP3) ||
				!emit_branch(negone_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
				!emit_branch(equal_branch, VitaA32::Condition::EQ))
			{
				return false;
			}

			if (!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP0,
					VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1,
					VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP4, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP4, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
				!m_code.EmitAddReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitClz(HOST_TMP4, HOST_TMP4) ||
				!m_code.EmitRsbImm32(HOST_TMP2, HOST_TMP4, 31) ||
				!m_code.EmitMovRegShiftReg(
					HOST_TMP4, HOST_TMP3, VitaA32::ShiftType::ASR, HOST_TMP2) ||
				!m_code.EmitMovRegShiftReg(
					HOST_TMP3, HOST_TMP4, VitaA32::ShiftType::LSL, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1,
					VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP4, HOST_TMP2) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP3, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(negone_branch, m_code.Size()) ||
				!m_code.EmitRsbImm32(HOST_TMP2, HOST_TMP0, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset, HOST_TMP3) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_zero_extended_word_as_doubleword(HOST_TMP2, HOST_TMP2,
					hi_offset, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1) ||
				!m_code.EmitMovImm8(HOST_TMP3, 0) ||
				!store_zero_extended_word_as_doubleword(HOST_TMP2, HOST_TMP3,
					lo_offset) ||
				!store_zero_extended_word_as_doubleword(HOST_TMP3, HOST_TMP3,
					hi_offset, HOST_TMP2) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(divone_branch, m_code.Size()) ||
				!store_signed_word_as_doubleword(HOST_TMP0, lo_offset, HOST_TMP1) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_zero_extended_word_as_doubleword(HOST_TMP2, HOST_TMP2,
					hi_offset, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(zero_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_zero_extended_word_as_doubleword(HOST_TMP2, HOST_TMP2,
					lo_offset, HOST_TMP3) ||
				!store_zero_extended_word_as_doubleword(HOST_TMP2, HOST_TMP2,
					hi_offset, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(divzero_branch, m_code.Size()) ||
				!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) ||
				!m_code.EmitCmpImm32(HOST_TMP0, 0) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::LT) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset, HOST_TMP3) ||
				!store_signed_word_as_doubleword(HOST_TMP0, hi_offset, HOST_TMP1))
			{
				return false;
			}

			return patch_branches(done_branches, done_branch_count,
				m_code.Size());
		};

		const auto emit_unsigned_direct_lane = [&](unsigned source_word,
												   unsigned dest_lane,
												   bool operands_preloaded) {
			const size_t lo_offset = LO_OFFSET + dest_lane * sizeof(u64);
			const size_t hi_offset = HI_OFFSET + dest_lane * sizeof(u64);
			BranchPatch divzero_branch{};
			BranchPatch zero_branch{};
			BranchPatch divone_branch{};
			BranchPatch below_branch{};
			BranchPatch equal_branch{};
			BranchPatch done_branches[6]{};
			unsigned done_branch_count = 0;

			if ((!operands_preloaded &&
					(!load_word(rs, source_word, HOST_TMP0) ||
						!load_word(rt, source_word, HOST_TMP1))) ||
				!m_code.EmitCmpImm32(HOST_TMP1, 0) ||
				!emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpImm32(HOST_TMP0, 0) ||
				!emit_branch(zero_branch, VitaA32::Condition::EQ) ||
				!EmitCmpImm32OrReg(HOST_TMP1, 1, HOST_TMP3) ||
				!emit_branch(divone_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
				!emit_branch(below_branch, VitaA32::Condition::CC) ||
				!emit_branch(equal_branch, VitaA32::Condition::EQ))
			{
				return false;
			}

			if (!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP1, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitClz(HOST_TMP2, HOST_TMP1) ||
				!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP2, 31) ||
				!m_code.EmitMovRegShiftReg(
					HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSR, HOST_TMP4) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP3, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1) ||
				!m_code.EmitMovImm8(HOST_TMP3, 0) ||
				!store_zero_extended_word_as_doubleword(HOST_TMP2, HOST_TMP3,
					lo_offset) ||
				!store_zero_extended_word_as_doubleword(HOST_TMP3, HOST_TMP3,
					hi_offset, HOST_TMP2) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(below_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_zero_extended_word_as_doubleword(HOST_TMP2, HOST_TMP2,
					lo_offset, HOST_TMP3) ||
				!store_signed_word_as_doubleword(HOST_TMP0, hi_offset, HOST_TMP1) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(divone_branch, m_code.Size()) ||
				!store_signed_word_as_doubleword(HOST_TMP0, lo_offset, HOST_TMP1) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_zero_extended_word_as_doubleword(HOST_TMP2, HOST_TMP2,
					hi_offset, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(zero_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_zero_extended_word_as_doubleword(HOST_TMP2, HOST_TMP2,
					lo_offset, HOST_TMP3) ||
				!store_zero_extended_word_as_doubleword(HOST_TMP2, HOST_TMP2,
					hi_offset, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(divzero_branch, m_code.Size()) ||
				!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset, HOST_TMP3) ||
				!store_signed_word_as_doubleword(HOST_TMP0, hi_offset, HOST_TMP1))
			{
				return false;
			}

			return patch_branches(done_branches, done_branch_count,
				m_code.Size());
		};

		const auto emit_store_active_word_lanes_as_doublewords =
			[&](size_t offset, unsigned value_qreg, unsigned sign_qreg,
				unsigned address_scratch) {
				return m_code.EmitVshrS32Q(sign_qreg, value_qreg, 31) &&
			           m_code.EmitVtrnI32Q(value_qreg, sign_qreg) &&
			           EmitStoreCpuRegsQ128(offset, value_qreg,
						   address_scratch);
			};

		const auto emit_zero_divisor_vector = [&]() {
			const int cached_rs_qreg = FindGprQCache(rs);
			const auto choose_qreg = [&](int avoid0, int avoid1) {
				for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
				{
					if (static_cast<int>(qreg) != avoid0 &&
						static_cast<int>(qreg) != avoid1 &&
						!IsGprQCacheQregResident(qreg))
					{
						return qreg;
					}
				}
				for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
				{
					if (static_cast<int>(qreg) != avoid0 &&
						static_cast<int>(qreg) != avoid1 &&
						!GprQCacheQregHasFutureQwordReadBeforeWrite(qreg))
					{
						return qreg;
					}
				}
				return 0u;
			};
			const unsigned source_qreg = cached_rs_qreg >= 0 ?
			                                 static_cast<unsigned>(cached_rs_qreg) :
			                                 0u;
			const unsigned lo_qreg =
				choose_qreg(static_cast<int>(source_qreg), -1);
			const unsigned sign_qreg =
				choose_qreg(static_cast<int>(source_qreg), static_cast<int>(lo_qreg));

			if (cached_rs_qreg < 0 &&
				!(ShouldLoadGprQ128SingleUseEntry(rs) ?
					  EmitLoadGprQ128SingleUseEntry(rs, source_qreg, HOST_TMP0) :
					  EmitLoadGprQ128(rs, source_qreg, HOST_TMP0)))
			{
				return false;
			}

			if (cached_rs_qreg >= 0 &&
				!PreserveGprQCacheGuestForFutureRead(rs, source_qreg,
					lo_qreg, sign_qreg, nullptr))
			{
				return false;
			}
			InvalidateGprQCacheForQreg(source_qreg);
			InvalidateGprQCacheForQreg(lo_qreg);
			InvalidateGprQCacheForQreg(sign_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedWordDivideVectorOps++;
			g_qemuMmiPackedWordDivideZeroDivisorVectorOps++;
			if (cached_rs_qreg >= 0)
				g_qemuGprQCacheDirectWordDivideZeroDivisorOps++;
#endif

			// PCSX2 owners: MMI.cpp::_PDIVW() and _PDIVUW(). With RT=$zero,
			// both active divisors are architecturally zero, so one vector path
			// writes LO's divide-by-zero result and keeps the sign-extended
			// dividend words in HI.
			const bool lo_ok = signed_divide ?
			                       (m_code.EmitVshrS32Q(lo_qreg, source_qreg, 31) &&
									   m_code.EmitVshlI32Q(lo_qreg, lo_qreg, 1) &&
									   m_code.EmitVmvnQ(lo_qreg, lo_qreg)) :
			                       (m_code.EmitVeorQ(lo_qreg, lo_qreg, lo_qreg) &&
									   m_code.EmitVmvnQ(lo_qreg, lo_qreg));
			return lo_ok &&
			       emit_store_active_word_lanes_as_doublewords(
					   LO_OFFSET, lo_qreg, sign_qreg, HOST_TMP1) &&
			       emit_store_active_word_lanes_as_doublewords(
					   HI_OFFSET, source_qreg, sign_qreg, HOST_TMP2);
		};

		const auto emit_zero_dividend_vector = [&]() {
			const int cached_rt_qreg = FindGprQCache(rt);
			const auto choose_qreg = [&](int avoid0, int avoid1, int avoid2) {
				for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
				{
					if (static_cast<int>(qreg) != avoid0 &&
						static_cast<int>(qreg) != avoid1 &&
						static_cast<int>(qreg) != avoid2 &&
						!IsGprQCacheQregResident(qreg))
					{
						return qreg;
					}
				}
				for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
				{
					if (static_cast<int>(qreg) != avoid0 &&
						static_cast<int>(qreg) != avoid1 &&
						static_cast<int>(qreg) != avoid2 &&
						!GprQCacheQregHasFutureQwordReadBeforeWrite(qreg))
					{
						return qreg;
					}
				}
				return 0u;
			};
			const unsigned rt_qreg = cached_rt_qreg >= 0 ?
			                             static_cast<unsigned>(cached_rt_qreg) :
			                             0u;
			const unsigned lo_qreg = choose_qreg(static_cast<int>(rt_qreg), -1, -1);
			const unsigned zero_qreg =
				choose_qreg(static_cast<int>(rt_qreg), static_cast<int>(lo_qreg), -1);
			const unsigned sign_qreg =
				choose_qreg(static_cast<int>(rt_qreg), static_cast<int>(lo_qreg),
					static_cast<int>(zero_qreg));

			if (cached_rt_qreg < 0 &&
				!(ShouldLoadGprQ128SingleUseEntry(rt) ?
					  EmitLoadGprQ128SingleUseEntry(rt, rt_qreg, HOST_TMP0) :
					  EmitLoadGprQ128(rt, rt_qreg, HOST_TMP0)))
			{
				return false;
			}

			if (cached_rt_qreg < 0)
				InvalidateGprQCacheForQreg(rt_qreg);
			InvalidateGprQCacheForQreg(lo_qreg);
			InvalidateGprQCacheForQreg(zero_qreg);
			InvalidateGprQCacheForQreg(sign_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedWordDivideVectorOps++;
			g_qemuMmiPackedWordDivideZeroDividendVectorOps++;
			if (cached_rt_qreg >= 0)
				g_qemuGprQCacheDirectWordDivideZeroDividendOps++;
#endif

			// PCSX2 owners: MMI.cpp::_PDIVW() and _PDIVUW(). With RS=$zero,
			// nonzero divisors produce LO/HI zero; zero divisors produce LO -1
			// and still leave HI zero. Compare the divisor vector once, then
			// store only the active word lanes as sign-extended doublewords.
			return m_code.EmitVeorQ(zero_qreg, zero_qreg, zero_qreg) &&
			       m_code.EmitVceqI32Q(lo_qreg, rt_qreg, zero_qreg) &&
			       emit_store_active_word_lanes_as_doublewords(
					   LO_OFFSET, lo_qreg, sign_qreg, HOST_TMP1) &&
			       EmitStoreCpuRegsQ128(HI_OFFSET, zero_qreg, HOST_TMP2);
		};

		const auto emit_signed_power_of_two_test =
			[&](unsigned divisor_reg, BranchPatch* scalar_branches,
				unsigned& scalar_branch_count) {
				if (scalar_branch_count + 2 > 4)
					return false;

				if (!m_code.EmitCmpImm32(divisor_reg, 0) ||
					!emit_branch(scalar_branches[scalar_branch_count++],
						VitaA32::Condition::EQ) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP4, divisor_reg,
						VitaA32::ShiftType::ASR, 31) ||
					!m_code.EmitEorReg(HOST_TMP5, divisor_reg, HOST_TMP4) ||
					!m_code.EmitSubReg(HOST_TMP5, HOST_TMP5, HOST_TMP4) ||
					!m_code.EmitSubImm8(HOST_TMP4, HOST_TMP5, 1) ||
					!m_code.EmitAndReg(HOST_TMP4, HOST_TMP5, HOST_TMP4, true) ||
					!emit_branch(scalar_branches[scalar_branch_count++],
						VitaA32::Condition::NE))
				{
					return false;
				}

				return true;
			};

		const auto emit_unsigned_power_of_two_test =
			[&](unsigned divisor_reg, BranchPatch* scalar_branches,
				unsigned& scalar_branch_count) {
				if (scalar_branch_count + 2 > 4)
					return false;

				if (!m_code.EmitCmpImm32(divisor_reg, 0) ||
					!emit_branch(scalar_branches[scalar_branch_count++],
						VitaA32::Condition::EQ) ||
					!m_code.EmitSubImm8(HOST_TMP4, divisor_reg, 1) ||
					!m_code.EmitAndReg(HOST_TMP4, divisor_reg, HOST_TMP4,
						true) ||
					!emit_branch(scalar_branches[scalar_branch_count++],
						VitaA32::Condition::NE))
				{
					return false;
				}

				return true;
			};

		const auto emit_signed_power_of_two_vector_from_loaded = [&]() {
			const int cached_rs_qreg = FindGprQCache(rs);
			bool used_qregs[8]{};
			const auto reserve_qreg = [&](unsigned qreg) {
				if (qreg < 8)
					used_qregs[qreg] = true;
			};
			const auto alloc_qreg = [&]() {
				for (unsigned qreg = 0; qreg < 8; qreg++)
				{
					if (!used_qregs[qreg])
					{
						used_qregs[qreg] = true;
						return qreg;
					}
				}
				return 0u;
			};

			const unsigned neon_rs = cached_rs_qreg >= 0 ?
			                             static_cast<unsigned>(cached_rs_qreg) :
			                             0u;
			reserve_qreg(neon_rs);
			const unsigned neon_sign = alloc_qreg();
			const unsigned neon_bias_mask = alloc_qreg();
			const unsigned neon_shift_right = alloc_qreg();
			const unsigned neon_quotient = alloc_qreg();
			const unsigned neon_product = alloc_qreg();
			const unsigned neon_divisor_sign = alloc_qreg();
			const unsigned neon_shift_left = alloc_qreg();

			const auto set_lane_config = [&](unsigned divisor_reg,
											 unsigned lane) {
				return m_code.EmitMovRegShiftImm(HOST_TMP4, divisor_reg,
						   VitaA32::ShiftType::ASR, 31) &&
				       EmitMoveCoreToQWordLane(neon_divisor_sign, lane, HOST_TMP4) &&
				       m_code.EmitEorReg(HOST_TMP5, divisor_reg, HOST_TMP4) &&
				       m_code.EmitSubReg(HOST_TMP5, HOST_TMP5, HOST_TMP4) &&
				       m_code.EmitSubImm8(HOST_TMP4, HOST_TMP5, 1) &&
				       EmitMoveCoreToQWordLane(neon_bias_mask, lane, HOST_TMP4) &&
				       m_code.EmitClz(HOST_TMP4, HOST_TMP5) &&
				       m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP4, 31) &&
				       EmitMoveCoreToQWordLane(neon_shift_left, lane, HOST_TMP4) &&
				       m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP4, 0) &&
				       EmitMoveCoreToQWordLane(neon_shift_right, lane, HOST_TMP4);
			};

			if (cached_rs_qreg < 0 &&
				!(ShouldLoadGprQ128SingleUseEntry(rs) ?
					  EmitLoadGprQ128SingleUseEntry(rs, neon_rs, HOST_TMP0) :
					  EmitLoadGprQ128(rs, neon_rs, HOST_TMP0)))
			{
				return false;
			}

			if (cached_rs_qreg < 0)
				InvalidateGprQCacheForQreg(neon_rs);
			InvalidateGprQCacheForQreg(neon_sign);
			InvalidateGprQCacheForQreg(neon_bias_mask);
			InvalidateGprQCacheForQreg(neon_shift_right);
			InvalidateGprQCacheForQreg(neon_quotient);
			InvalidateGprQCacheForQreg(neon_product);
			InvalidateGprQCacheForQreg(neon_divisor_sign);
			InvalidateGprQCacheForQreg(neon_shift_left);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedWordDivideVectorOps++;
			if (cached_rs_qreg >= 0)
				g_qemuGprQCacheDirectWordDividePowerOfTwoOps++;
#endif

			// PCSX2 owner: MMI.cpp::_PDIVW(). For signed power-of-two
			// divisors, bias negative dividends before the arithmetic shift
			// so the quotient still truncates toward zero.
			return m_code.EmitVeorQ(neon_bias_mask, neon_bias_mask,
					   neon_bias_mask) &&
			       m_code.EmitVeorQ(neon_shift_right, neon_shift_right,
					   neon_shift_right) &&
			       m_code.EmitVeorQ(neon_divisor_sign, neon_divisor_sign,
					   neon_divisor_sign) &&
			       m_code.EmitVeorQ(neon_shift_left, neon_shift_left,
					   neon_shift_left) &&
			       set_lane_config(HOST_TMP1, 0) &&
			       set_lane_config(HOST_TMP3, 2) &&
			       m_code.EmitVshrS32Q(neon_sign, neon_rs, 31) &&
			       m_code.EmitVandQ(neon_sign, neon_sign, neon_bias_mask) &&
			       m_code.EmitVaddI32Q(neon_quotient, neon_rs, neon_sign) &&
			       m_code.EmitVshlS32Q(neon_quotient, neon_quotient,
					   neon_shift_right) &&
			       m_code.EmitVeorQ(neon_quotient, neon_quotient,
					   neon_divisor_sign) &&
			       m_code.EmitVsubI32Q(neon_quotient, neon_quotient,
					   neon_divisor_sign) &&
			       m_code.EmitVshlS32Q(neon_product, neon_quotient,
					   neon_shift_left) &&
			       m_code.EmitVeorQ(neon_product, neon_product,
					   neon_divisor_sign) &&
			       m_code.EmitVsubI32Q(neon_product, neon_product,
					   neon_divisor_sign) &&
			       m_code.EmitVsubI32Q(neon_product, neon_rs, neon_product) &&
			       emit_store_active_word_lanes_as_doublewords(
					   LO_OFFSET, neon_quotient, neon_sign, HOST_TMP0) &&
			       emit_store_active_word_lanes_as_doublewords(
					   HI_OFFSET, neon_product, neon_sign, HOST_TMP1);
		};

		const auto emit_unsigned_power_of_two_vector_from_loaded = [&]() {
			const int cached_rs_qreg = FindGprQCache(rs);
			bool used_qregs[8]{};
			const auto reserve_qreg = [&](unsigned qreg) {
				if (qreg < 8)
					used_qregs[qreg] = true;
			};
			const auto alloc_qreg = [&]() {
				for (unsigned qreg = 0; qreg < 8; qreg++)
				{
					if (!used_qregs[qreg])
					{
						used_qregs[qreg] = true;
						return qreg;
					}
				}
				return 0u;
			};

			const unsigned neon_rs = cached_rs_qreg >= 0 ?
			                             static_cast<unsigned>(cached_rs_qreg) :
			                             0u;
			reserve_qreg(neon_rs);
			const unsigned neon_mask = alloc_qreg();
			const unsigned neon_shift_right = alloc_qreg();
			const unsigned neon_quotient = alloc_qreg();
			const unsigned neon_remainder = alloc_qreg();
			const unsigned neon_sign = alloc_qreg();

			const auto set_lane_config = [&](unsigned divisor_reg,
											 unsigned lane) {
				return m_code.EmitSubImm8(HOST_TMP4, divisor_reg, 1) &&
				       EmitMoveCoreToQWordLane(neon_mask, lane, HOST_TMP4) &&
				       m_code.EmitClz(HOST_TMP4, divisor_reg) &&
				       m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP4, 31) &&
				       m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP4, 0) &&
				       EmitMoveCoreToQWordLane(neon_shift_right, lane, HOST_TMP4);
			};

			if (cached_rs_qreg < 0 &&
				!(ShouldLoadGprQ128SingleUseEntry(rs) ?
					  EmitLoadGprQ128SingleUseEntry(rs, neon_rs, HOST_TMP0) :
					  EmitLoadGprQ128(rs, neon_rs, HOST_TMP0)))
			{
				return false;
			}

			if (cached_rs_qreg < 0)
				InvalidateGprQCacheForQreg(neon_rs);
			InvalidateGprQCacheForQreg(neon_mask);
			InvalidateGprQCacheForQreg(neon_shift_right);
			InvalidateGprQCacheForQreg(neon_quotient);
			InvalidateGprQCacheForQreg(neon_remainder);
			InvalidateGprQCacheForQreg(neon_sign);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedWordDivideVectorOps++;
			if (cached_rs_qreg >= 0)
				g_qemuGprQCacheDirectWordDividePowerOfTwoOps++;
#endif

			// PCSX2 owner: MMI.cpp::_PDIVUW(). Power-of-two unsigned
			// divisors are quotient=dividend>>log2(divisor),
			// remainder=dividend&(divisor-1).
			return m_code.EmitVeorQ(neon_mask, neon_mask, neon_mask) &&
			       m_code.EmitVeorQ(neon_shift_right, neon_shift_right,
					   neon_shift_right) &&
			       set_lane_config(HOST_TMP1, 0) &&
			       set_lane_config(HOST_TMP3, 2) &&
			       m_code.EmitVshlU32Q(neon_quotient, neon_rs,
					   neon_shift_right) &&
			       m_code.EmitVandQ(neon_remainder, neon_rs, neon_mask) &&
			       emit_store_active_word_lanes_as_doublewords(
					   LO_OFFSET, neon_quotient, neon_sign, HOST_TMP0) &&
			       emit_store_active_word_lanes_as_doublewords(
					   HI_OFFSET, neon_remainder, neon_sign, HOST_TMP1);
		};

		const auto emit_power_of_two_vector_path =
			[&](bool signed_path, BranchPatch& done_branch,
				bool& has_done_branch) {
				BranchPatch scalar_branches[4]{};
				unsigned scalar_branch_count = 0;
				const bool tests_ok =
					signed_path ? (emit_signed_power_of_two_test(HOST_TMP1,
									   scalar_branches,
									   scalar_branch_count) &&
									  emit_signed_power_of_two_test(
										  HOST_TMP3, scalar_branches, scalar_branch_count)) :
								  (emit_unsigned_power_of_two_test(
									   HOST_TMP1, scalar_branches,
									   scalar_branch_count) &&
									  emit_unsigned_power_of_two_test(
										  HOST_TMP3, scalar_branches,
										  scalar_branch_count));
				if (!tests_ok)
					return false;

				const bool vector_ok =
					signed_path ? emit_signed_power_of_two_vector_from_loaded() : emit_unsigned_power_of_two_vector_from_loaded();
				if (!vector_ok ||
					!emit_branch(done_branch, VitaA32::Condition::AL))
				{
					return false;
				}

				has_done_branch = true;
				return patch_branches(scalar_branches, scalar_branch_count,
					m_code.Size());
			};

		if (rt == 0)
			return emit_zero_divisor_vector();

		if (rs == 0)
			return emit_zero_dividend_vector();

		if (!load_word(rs, 0, HOST_TMP0) || !load_word(rt, 0, HOST_TMP1) ||
			!load_word(rs, 2, HOST_TMP2) || !load_word(rt, 2, HOST_TMP3))
		{
			return false;
		}

		if (signed_divide)
		{
			if (!emit_signed_fast_guard(HOST_TMP0, HOST_TMP1) ||
				!emit_signed_fast_guard(HOST_TMP2, HOST_TMP3))
			{
				return false;
			}
		}
		else if (!emit_unsigned_fast_guard(HOST_TMP0, HOST_TMP1) ||
				 !emit_unsigned_fast_guard(HOST_TMP2, HOST_TMP3))
		{
			return false;
		}

		BranchPatch vector_done_branch{};
		bool has_vector_done_branch = false;
		if (signed_divide)
		{
			// The guard path loaded lane 0 into HOST_TMP0/HOST_TMP1 and lane
			// 2 into HOST_TMP2/HOST_TMP3. If both divisors are signed
			// powers of two, emit one NEON path for both active words before
			// falling back to the scalar per-lane direct forms.
			if (!emit_power_of_two_vector_path(true, vector_done_branch,
					has_vector_done_branch) ||
				!emit_signed_direct_lane(0, 0, true) ||
				!emit_signed_direct_lane(2, 1, false))
			{
				return false;
			}
		}
		else if (!emit_power_of_two_vector_path(false, vector_done_branch,
					 has_vector_done_branch) ||
				 !emit_unsigned_direct_lane(0, 0, true) ||
				 !emit_unsigned_direct_lane(2, 1, false))
		{
			return false;
		}

		BranchPatch done_branch{};
		if (!emit_branch(done_branch, VitaA32::Condition::AL))
			return false;

		if (!patch_branches(fallback_branches, fallback_branch_count,
				m_code.Size()) ||
			!(signed_divide ? emit_signed_general_lane(0, 0, true) :
							  emit_unsigned_general_lane(0, 0, true)) ||
			!(signed_divide ? emit_signed_general_lane(2, 1, false) :
							  emit_unsigned_general_lane(2, 1, false)))
		{
			return false;
		}

		const size_t done_target = m_code.Size();
		if (has_vector_done_branch &&
			!patch_branch(vector_done_branch, done_target))
		{
			return false;
		}

		return patch_branch(done_branch, done_target);
	}

	bool BlockCompiler::EmitPackedWordByHalfwordDivide(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		struct BranchPatch
		{
			size_t offset = static_cast<size_t>(-1);
			VitaA32::Condition condition = VitaA32::Condition::AL;
		};

		const auto emit_branch = [this](BranchPatch& patch,
									 VitaA32::Condition condition) {
			patch.offset = m_code.EmitBranchPlaceholder(condition);
			patch.condition = condition;
			return patch.offset != static_cast<size_t>(-1);
		};

		const auto patch_branch = [this](const BranchPatch& patch,
									  size_t target) {
			return m_code.PatchBranch(patch.offset, target, patch.condition);
		};

		const auto patch_branches =
			[patch_branch](const BranchPatch* branches, unsigned count,
				size_t target) {
				for (unsigned i = 0; i < count; i++)
				{
					if (!patch_branch(branches[i], target))
						return false;
				}
				return true;
			};

		const auto reserve_qreg = [](auto& used_qregs, unsigned qreg) {
			if (qreg < MAX_GPR_QCACHE)
				used_qregs[qreg] = true;
		};

		const auto alloc_cold_qreg = [this](auto& used_qregs) {
			u32 used_qreg_mask = 0;
			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
				used_qreg_mask |= static_cast<u32>(used_qregs[qreg]) << qreg;

			const unsigned qreg = SelectGprQCacheScratchQreg(used_qreg_mask);
			if (qreg < MAX_GPR_QCACHE)
				used_qregs[qreg] = true;
			return qreg;
		};

		const auto emit_unsigned_shift_subtract_divide =
			[&](unsigned dividend_reg, unsigned divisor_reg,
				unsigned quotient_reg, unsigned remainder_reg) {
				BranchPatch below_branch{};
				BranchPatch equal_branch{};
				BranchPatch done_branches[2]{};
				unsigned done_branch_count = 0;

				if (!m_code.EmitMovImm8(quotient_reg, 0) ||
					!m_code.EmitMovRegShiftImm(remainder_reg, dividend_reg,
						VitaA32::ShiftType::LSL, 0) ||
					!m_code.EmitCmpReg(dividend_reg, divisor_reg) ||
					!emit_branch(below_branch, VitaA32::Condition::CC) ||
					!emit_branch(equal_branch, VitaA32::Condition::EQ))
				{
					return false;
				}

				if (!m_code.EmitClz(quotient_reg, divisor_reg) ||
					!m_code.EmitClz(dividend_reg, dividend_reg) ||
					!m_code.EmitSubReg(quotient_reg, quotient_reg,
						dividend_reg) ||
					!m_code.EmitMovRegShiftReg(dividend_reg, divisor_reg,
						VitaA32::ShiftType::LSL, quotient_reg) ||
					!m_code.EmitMovImm8(divisor_reg, 1) ||
					!m_code.EmitMovRegShiftReg(divisor_reg, divisor_reg,
						VitaA32::ShiftType::LSL, quotient_reg) ||
					!m_code.EmitMovImm8(quotient_reg, 0))
				{
					return false;
				}

				const size_t loop_start = m_code.Size();
				BranchPatch skip_subtract_branch{};
				BranchPatch loop_branch{};
				if (!m_code.EmitCmpReg(remainder_reg, dividend_reg) ||
					!emit_branch(skip_subtract_branch,
						VitaA32::Condition::CC) ||
					!m_code.EmitSubReg(remainder_reg, remainder_reg,
						dividend_reg) ||
					!m_code.EmitOrrReg(quotient_reg, quotient_reg,
						divisor_reg) ||
					!patch_branch(skip_subtract_branch, m_code.Size()) ||
					!m_code.EmitMovRegShiftImm(divisor_reg, divisor_reg,
						VitaA32::ShiftType::LSR, 1, true) ||
					!m_code.EmitMovRegShiftImm(dividend_reg, dividend_reg,
						VitaA32::ShiftType::LSR, 1) ||
					!emit_branch(loop_branch, VitaA32::Condition::NE) ||
					!patch_branch(loop_branch, loop_start) ||
					!emit_branch(done_branches[done_branch_count++],
						VitaA32::Condition::AL))
				{
					return false;
				}

				if (!patch_branch(equal_branch, m_code.Size()) ||
					!m_code.EmitMovImm8(quotient_reg, 1) ||
					!m_code.EmitMovImm8(remainder_reg, 0) ||
					!emit_branch(done_branches[done_branch_count++],
						VitaA32::Condition::AL) ||
					!patch_branch(below_branch, m_code.Size()))
				{
					return false;
				}

				return patch_branches(done_branches, done_branch_count,
					m_code.Size());
			};

		const auto load_divisor = [&]() {
			if (rt == 0)
				return m_code.EmitMovImm8(HOST_TMP1, 0);

			unsigned rt_low;
			return EmitGprWordOperand(rt, 0, HOST_TMP1, &rt_low) &&
			       m_code.EmitUxth(HOST_TMP1, rt_low);
		};

		const auto emit_arbitrary_divisor_lane = [&](unsigned lane,
													 bool divisor_known, s32 known_divisor, unsigned shared_divisor_reg) {
			const size_t lo_offset = LO_OFFSET + lane * sizeof(u32);
			const size_t hi_offset = HI_OFFSET + lane * sizeof(u32);

			if (!EmitLoadGprWord(rs, lane, HOST_TMP0))
			{
				return false;
			}

			if (divisor_known)
			{
				if (!m_code.EmitMovImm32(HOST_TMP1,
						static_cast<u32>(known_divisor)))
				{
					return false;
				}
			}
			else if (shared_divisor_reg != 0)
			{
				if (!m_code.EmitMovRegShiftImm(HOST_TMP1, shared_divisor_reg,
						VitaA32::ShiftType::LSL, 0))
				{
					return false;
				}
			}
			else if (!load_divisor() ||
					 !m_code.EmitSxth(HOST_TMP1, HOST_TMP1))
			{
				return false;
			}

			// PCSX2 owner: MMI.cpp::_PDIVBW(). The arbitrary signed
			// word/signed-halfword path uses C truncation toward zero, with
			// HI receiving the signed remainder as a 32-bit lane.
			if (!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0,
					VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1,
					VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP4, HOST_TMP2, HOST_TMP5) ||
				!m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP5) ||
				!m_code.EmitSubReg(HOST_TMP0, HOST_TMP0, HOST_TMP5) ||
				!m_code.EmitEorReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) ||
				!emit_unsigned_shift_subtract_divide(HOST_TMP0, HOST_TMP1,
					HOST_TMP2, HOST_TMP3) ||
				!m_code.EmitEorReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
				!m_code.EmitEorReg(HOST_TMP3, HOST_TMP3, HOST_TMP5) ||
				!m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_TMP5) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS,
					static_cast<u16>(lo_offset)) ||
				!m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS,
					static_cast<u16>(hi_offset)))
			{
				return false;
			}

			return true;
		};

		const auto emit_divzero_vector = [&]() {
			const int cached_rs_qreg = FindGprQCache(rs);
			bool used_qregs[MAX_GPR_QCACHE]{};
			unsigned neon_rs = cached_rs_qreg >= 0 ?
			                       static_cast<unsigned>(cached_rs_qreg) :
			                       MAX_GPR_QCACHE;
			if (neon_rs >= MAX_GPR_QCACHE)
				neon_rs = alloc_cold_qreg(used_qregs);
			reserve_qreg(used_qregs, neon_rs);
			const unsigned neon_lo = alloc_cold_qreg(used_qregs);

			if (neon_rs >= MAX_GPR_QCACHE || neon_lo >= MAX_GPR_QCACHE)
				return false;

			if (cached_rs_qreg < 0 &&
				!(ShouldLoadGprQ128SingleUseEntry(rs) ?
					  EmitLoadGprQ128SingleUseEntry(rs, neon_rs, HOST_TMP0) :
					  EmitLoadGprQ128(rs, neon_rs, HOST_TMP0)))
			{
				return false;
			}

			if (cached_rs_qreg < 0)
				InvalidateGprQCacheForQreg(neon_rs);
			InvalidateGprQCacheForQreg(neon_lo);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedWordByHalfwordDivideVectorOps++;
			if (cached_rs_qreg >= 0)
				g_qemuGprQCacheDirectWordByHalfwordDivideOps++;
#endif

			// PCSX2 owner: MMI.cpp::_PDIVBW(). Dividing by zero keeps the
			// dividend in HI and writes LO = (dividend < 0) ? 1 : -1.
			return EmitStoreCpuRegsQ128(HI_OFFSET, neon_rs, HOST_TMP1) &&
			       m_code.EmitVshrS32Q(neon_lo, neon_rs, 31) &&
			       m_code.EmitVshlI32Q(neon_lo, neon_lo, 1) &&
			       m_code.EmitVmvnQ(neon_lo, neon_lo) &&
			       EmitStoreCpuRegsQ128(LO_OFFSET, neon_lo, HOST_TMP2);
		};

		const auto emit_unit_divisor_vector = [&](bool negate) {
			const int cached_rs_qreg = FindGprQCache(rs);
			bool used_qregs[MAX_GPR_QCACHE]{};
			unsigned neon_rs = cached_rs_qreg >= 0 ?
			                       static_cast<unsigned>(cached_rs_qreg) :
			                       MAX_GPR_QCACHE;
			if (neon_rs >= MAX_GPR_QCACHE)
				neon_rs = alloc_cold_qreg(used_qregs);
			reserve_qreg(used_qregs, neon_rs);
			const unsigned neon_zero = alloc_cold_qreg(used_qregs);
			const unsigned neon_quotient =
				negate ? alloc_cold_qreg(used_qregs) : neon_rs;

			if (neon_rs >= MAX_GPR_QCACHE || neon_zero >= MAX_GPR_QCACHE ||
				neon_quotient >= MAX_GPR_QCACHE)
			{
				return false;
			}

			if (cached_rs_qreg < 0 &&
				!(ShouldLoadGprQ128SingleUseEntry(rs) ?
					  EmitLoadGprQ128SingleUseEntry(rs, neon_rs, HOST_TMP0) :
					  EmitLoadGprQ128(rs, neon_rs, HOST_TMP0)))
			{
				return false;
			}

			if (cached_rs_qreg < 0)
				InvalidateGprQCacheForQreg(neon_rs);
			InvalidateGprQCacheForQreg(neon_zero);
			if (negate)
				InvalidateGprQCacheForQreg(neon_quotient);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedWordByHalfwordDivideVectorOps++;
			if (cached_rs_qreg >= 0)
				g_qemuGprQCacheDirectWordByHalfwordDivideOps++;
#endif

			// PCSX2 owner: MMI.cpp::_PDIVBW(). Divisors +/-1 write zero HI;
			// -1 wraps INT_MIN to itself in the 32-bit quotient lane.
			return m_code.EmitVeorQ(neon_zero, neon_zero, neon_zero) &&
			       EmitStoreCpuRegsQ128(HI_OFFSET, neon_zero, HOST_TMP1) &&
			       (!negate || m_code.EmitVnegS32Q(neon_quotient, neon_rs)) &&
			       EmitStoreCpuRegsQ128(LO_OFFSET, neon_quotient, HOST_TMP2);
		};

		const auto emit_zero_dividend_vector =
			[&](bool divisor_known, u32 known_divisor_low) {
				bool used_qregs[MAX_GPR_QCACHE]{};
				const unsigned NEON_ZERO = alloc_cold_qreg(used_qregs);
				const unsigned NEON_LO = alloc_cold_qreg(used_qregs);

				if (NEON_ZERO >= MAX_GPR_QCACHE || NEON_LO >= MAX_GPR_QCACHE)
					return false;

				InvalidateGprQCacheForQreg(NEON_ZERO);
				InvalidateGprQCacheForQreg(NEON_LO);
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuMmiPackedWordByHalfwordDivideVectorOps++;
				g_qemuMmiPackedWordByHalfwordDivideZeroDividendVectorOps++;
#endif

				const auto emit_zero_lo = [&]() {
					return EmitStoreCpuRegsQ128(LO_OFFSET, NEON_ZERO, HOST_TMP1);
				};

				const auto emit_divzero_lo = [&]() {
					return m_code.EmitVmvnQ(NEON_LO, NEON_ZERO) &&
				           EmitStoreCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP1);
				};

				if (!m_code.EmitVeorQ(NEON_ZERO, NEON_ZERO, NEON_ZERO) ||
					!EmitStoreCpuRegsQ128(HI_OFFSET, NEON_ZERO, HOST_TMP0))
				{
					return false;
				}

				// PCSX2 owner: MMI.cpp::_PDIVBW(). With RS=$zero, HI is always
				// zero and LO is only all-ones when the scalar halfword divisor is
				// zero; otherwise quotient and remainder are both zero.
				if (divisor_known)
				{
					return (static_cast<u16>(known_divisor_low) == 0) ?
				               emit_divzero_lo() :
				               emit_zero_lo();
				}

				BranchPatch divzero_branch{};
				BranchPatch done_branch{};
				if (!load_divisor() || !m_code.EmitCmpImm32(HOST_TMP1, 0) ||
					!emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
					!emit_zero_lo() ||
					!emit_branch(done_branch, VitaA32::Condition::AL))
				{
					return false;
				}

				return patch_branch(divzero_branch, m_code.Size()) &&
			           emit_divzero_lo() &&
			           patch_branch(done_branch, m_code.Size());
			};

		// PCSX2 owner: MMI.cpp::_PDIVBW() does signed word / signed halfword,
		// with C truncation toward zero and HI = dividend - quotient *
		// divisor.
		const auto emit_power_of_two_vector = [&]() {
			const int cached_rs_qreg = FindGprQCache(rs);
			bool used_qregs[MAX_GPR_QCACHE]{};
			unsigned neon_rs = cached_rs_qreg >= 0 ?
			                       static_cast<unsigned>(cached_rs_qreg) :
			                       MAX_GPR_QCACHE;
			if (neon_rs >= MAX_GPR_QCACHE)
				neon_rs = alloc_cold_qreg(used_qregs);
			reserve_qreg(used_qregs, neon_rs);
			const unsigned neon_sign = alloc_cold_qreg(used_qregs);
			const unsigned neon_bias_mask = alloc_cold_qreg(used_qregs);
			const unsigned neon_shift_right = alloc_cold_qreg(used_qregs);
			const unsigned neon_quotient = alloc_cold_qreg(used_qregs);
			const unsigned neon_product = alloc_cold_qreg(used_qregs);
			const unsigned neon_divisor_sign = alloc_cold_qreg(used_qregs);
			const unsigned neon_shift_left = alloc_cold_qreg(used_qregs);

			if (neon_rs >= MAX_GPR_QCACHE || neon_sign >= MAX_GPR_QCACHE ||
				neon_bias_mask >= MAX_GPR_QCACHE ||
				neon_shift_right >= MAX_GPR_QCACHE ||
				neon_quotient >= MAX_GPR_QCACHE ||
				neon_product >= MAX_GPR_QCACHE ||
				neon_divisor_sign >= MAX_GPR_QCACHE ||
				neon_shift_left >= MAX_GPR_QCACHE)
			{
				return false;
			}

			const auto broadcast_i32 = [this](unsigned qreg,
										   unsigned host_reg) {
				return m_code.EmitVdupI32QFromCore(qreg, host_reg);
			};

			if (!m_code.EmitClz(HOST_TMP5, HOST_TMP3) ||
				!m_code.EmitRsbImm32(HOST_TMP5, HOST_TMP5, 31) ||
				!m_code.EmitSubImm8(HOST_TMP3, HOST_TMP3, 1) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP1,
					VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitRsbImm32(HOST_TMP2, HOST_TMP5, 0) ||
				(cached_rs_qreg < 0 &&
				 !(ShouldLoadGprQ128SingleUseEntry(rs) ?
					   EmitLoadGprQ128SingleUseEntry(rs, neon_rs, HOST_TMP0) :
					   EmitLoadGprQ128(rs, neon_rs, HOST_TMP0))))
			{
				return false;
			}

			if (cached_rs_qreg < 0)
				InvalidateGprQCacheForQreg(neon_rs);
			InvalidateGprQCacheForQreg(neon_sign);
			InvalidateGprQCacheForQreg(neon_bias_mask);
			InvalidateGprQCacheForQreg(neon_shift_right);
			InvalidateGprQCacheForQreg(neon_quotient);
			InvalidateGprQCacheForQreg(neon_product);
			InvalidateGprQCacheForQreg(neon_divisor_sign);
			InvalidateGprQCacheForQreg(neon_shift_left);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedWordByHalfwordDivideVectorOps++;
			if (cached_rs_qreg >= 0)
				g_qemuGprQCacheDirectWordByHalfwordDivideOps++;
#endif

			return broadcast_i32(neon_bias_mask, HOST_TMP3) &&
			       broadcast_i32(neon_shift_right, HOST_TMP2) &&
			       broadcast_i32(neon_divisor_sign, HOST_TMP4) &&
			       broadcast_i32(neon_shift_left, HOST_TMP5) &&
			       m_code.EmitVshrS32Q(neon_sign, neon_rs, 31) &&
			       m_code.EmitVandQ(neon_sign, neon_sign, neon_bias_mask) &&
			       m_code.EmitVaddI32Q(neon_quotient, neon_rs, neon_sign) &&
			       m_code.EmitVshlS32Q(neon_quotient, neon_quotient,
					   neon_shift_right) &&
			       m_code.EmitVeorQ(neon_quotient, neon_quotient,
					   neon_divisor_sign) &&
			       m_code.EmitVsubI32Q(neon_quotient, neon_quotient,
					   neon_divisor_sign) &&
			       EmitStoreCpuRegsQ128(LO_OFFSET, neon_quotient, HOST_TMP0) &&
			       m_code.EmitVshlS32Q(neon_product, neon_quotient,
					   neon_shift_left) &&
			       m_code.EmitVeorQ(neon_product, neon_product,
					   neon_divisor_sign) &&
			       m_code.EmitVsubI32Q(neon_product, neon_product,
					   neon_divisor_sign) &&
			       m_code.EmitVsubI32Q(neon_product, neon_rs, neon_product) &&
			       EmitStoreCpuRegsQ128(HI_OFFSET, neon_product, HOST_TMP1);
		};

		u32 known_rt_low = 0;
		const bool divisor_known = TryGetKnownGprLow(rt, &known_rt_low);
		if (rs == 0)
		{
			return emit_zero_dividend_vector(divisor_known, known_rt_low);
		}

		if (divisor_known)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedWordByHalfwordDivideKnownDivisorFastPaths++;
#endif
			const u16 raw_divisor = static_cast<u16>(known_rt_low);
			if (raw_divisor == 0)
				return emit_divzero_vector();
			if (raw_divisor == 1)
				return emit_unit_divisor_vector(false);
			if (raw_divisor == 0xffffu)
				return emit_unit_divisor_vector(true);

			const s32 signed_divisor =
				static_cast<s32>(static_cast<s16>(raw_divisor));
			const u32 divisor_sign =
				(signed_divisor < 0) ? 0xffffffffu : 0;
			const u32 abs_divisor =
				(static_cast<u32>(signed_divisor) ^ divisor_sign) -
				divisor_sign;
				if ((abs_divisor & (abs_divisor - 1)) == 0)
				{
					return m_code.EmitMovImm32(HOST_TMP1,
							   static_cast<u32>(signed_divisor)) &&
					       m_code.EmitMovImm32(HOST_TMP3, abs_divisor) &&
					       emit_power_of_two_vector();
				}

#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuMmiPackedWordByHalfwordDivideKnownArbitraryDivisors++;
#endif
				return m_code.EmitMovImm32(HOST_BRANCH_STATE,
						   static_cast<u32>(signed_divisor)) &&
				       emit_arbitrary_divisor_lane(0, false, 0, HOST_BRANCH_STATE) &&
			       emit_arbitrary_divisor_lane(1, false, 0, HOST_BRANCH_STATE) &&
			       emit_arbitrary_divisor_lane(2, false, 0, HOST_BRANCH_STATE) &&
			       emit_arbitrary_divisor_lane(3, false, 0, HOST_BRANCH_STATE);
		}

		// PCSX2 owner: x86/iR5900Analysis.cpp::recBackpropMMI() gives
		// PDIVBW one 128-bit RS live range across the operation. Runtime
		// divisors fan out to four NEON edge-case bodies plus the scalar
		// arbitrary-divisor body on Cortex-A9; materialize RS once before that
		// fan-out so every body sees the same resident dividend instead of
		// emitting its own qword reload. The scalar body's word reads are then
		// extracted from this qreg by EmitLoadGprWord().
		const int cached_dividend_qreg = FindGprQCache(rs);
		const unsigned dividend_qreg = cached_dividend_qreg >= 0 ?
			static_cast<unsigned>(cached_dividend_qreg) :
			SelectGprQCacheScratchQreg();
		if (dividend_qreg >= MAX_GPR_QCACHE ||
			(cached_dividend_qreg < 0 &&
			 !EmitLoadGprQ128(rs, dividend_qreg, HOST_TMP0)))
		{
			return false;
		}

		BranchPatch divzero_branch{};
		BranchPatch divone_branch{};
		BranchPatch negone_branch{};
		BranchPatch power_of_two_branch{};
		BranchPatch fallback_branch{};
		BranchPatch done_branches[4]{};
		unsigned done_branch_count = 0;

		if (!load_divisor() || !m_code.EmitCmpImm32(HOST_TMP1, 0) ||
			!emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
			!EmitCmpImm32OrReg(HOST_TMP1, 1, HOST_TMP4) ||
			!emit_branch(divone_branch, VitaA32::Condition::EQ) ||
			!EmitCmpImm32OrReg(HOST_TMP1, 0xffffu, HOST_TMP4) ||
			!emit_branch(negone_branch, VitaA32::Condition::EQ) ||
			!m_code.EmitSxth(HOST_TMP1, HOST_TMP1) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1,
				VitaA32::ShiftType::ASR, 31) ||
			!m_code.EmitEorReg(HOST_TMP3, HOST_TMP1, HOST_TMP2) ||
			!m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
			!m_code.EmitSubImm8(HOST_TMP4, HOST_TMP3, 1) ||
			!m_code.EmitAndReg(HOST_TMP4, HOST_TMP3, HOST_TMP4, true) ||
			!emit_branch(power_of_two_branch, VitaA32::Condition::EQ) ||
			!emit_branch(fallback_branch, VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(divzero_branch, m_code.Size()) ||
			!emit_divzero_vector() ||
			!emit_branch(done_branches[done_branch_count++],
				VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(divone_branch, m_code.Size()) ||
			!emit_unit_divisor_vector(false) ||
			!emit_branch(done_branches[done_branch_count++],
				VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(negone_branch, m_code.Size()) ||
			!emit_unit_divisor_vector(true) ||
			!emit_branch(done_branches[done_branch_count++],
				VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(power_of_two_branch, m_code.Size()) ||
			!emit_power_of_two_vector() ||
			!emit_branch(done_branches[done_branch_count++],
				VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(fallback_branch, m_code.Size()) ||
			!m_code.EmitMovRegShiftImm(HOST_BRANCH_STATE, HOST_TMP1,
				VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuMmiPackedWordByHalfwordDivideSharedDivisorReloadsElided += 4;
#endif
		if (!emit_arbitrary_divisor_lane(0, false, 0, HOST_BRANCH_STATE) ||
			!emit_arbitrary_divisor_lane(1, false, 0, HOST_BRANCH_STATE) ||
			!emit_arbitrary_divisor_lane(2, false, 0, HOST_BRANCH_STATE) ||
			!emit_arbitrary_divisor_lane(3, false, 0, HOST_BRANCH_STATE))
		{
			return false;
		}

		return patch_branches(done_branches, done_branch_count,
			m_code.Size());
	}

	bool BlockCompiler::EmitPackedSignedWordMultiplyAccumulate(u32 op, bool subtract)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);

		const auto load_word = [this](unsigned guest_reg, unsigned word, unsigned host_reg) {
			return EmitLoadGprWord(guest_reg, word, host_reg);
		};

		const auto store_signed_word_as_doubleword = [this](unsigned value_reg, size_t offset) {
			return m_code.EmitMovRegShiftImm(HOST_TMP0, value_reg, VitaA32::ShiftType::ASR, 31) &&
			       m_code.EmitStrImm12(value_reg, HOST_CPU_REGS, static_cast<u16>(offset)) &&
			       m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
		};

		const auto emit_zero_product_accumulator_vector = [&]() {
			constexpr unsigned NEON_LO = 0;
			constexpr unsigned NEON_HI = 1;
			constexpr unsigned NEON_LO_SIGN = 2;
			constexpr unsigned NEON_HI_SIGN = 3;

			InvalidateGprQCacheForQreg(NEON_LO);
			InvalidateGprQCacheForQreg(NEON_HI);
			InvalidateGprQCacheForQreg(NEON_LO_SIGN);
			InvalidateGprQCacheForQreg(NEON_HI_SIGN);

			if (!EmitLoadCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) ||
				!EmitLoadCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1) ||
				!m_code.EmitVshrS32Q(NEON_LO_SIGN, NEON_LO, 31) ||
				!m_code.EmitVshrS32Q(NEON_HI_SIGN, NEON_HI, 31) ||
				!m_code.EmitVtrnI32Q(NEON_LO, NEON_LO_SIGN) ||
				!m_code.EmitVtrnI32Q(NEON_HI, NEON_HI_SIGN) ||
				!EmitStoreCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) ||
				!EmitStoreCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1))
			{
				return false;
			}

			if (rd == 0)
				return true;

			return m_code.EmitVtrnI32Q(NEON_LO, NEON_HI) &&
			       EmitStoreGprQ128(rd, NEON_LO, HOST_TMP2);
		};

		if ((rs == 0 || rt == 0) && (subtract || (rs == 0 && rt == 0)))
		{
			// PCSX2 owners: MMI.cpp::_PMSUBW() and _PMADDW(). A zero product
			// still sign-extends the active LO/HI words into their 64-bit lanes.
			// PMADDW's lower-lane quirk is only compile-time impossible when both
			// sources are architectural $zero; PMSUBW has no quirk path.
			return emit_zero_product_accumulator_vector();
		}

		const auto emit_divide_signed_64_by_u32_max = [this]() {
			if (!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t negative_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::LT);
			if (negative_branch == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMvnReg(HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitCmpReg(HOST_TMP4, HOST_TMP0))
			{
				return false;
			}

			const size_t positive_no_increment = m_code.EmitBranchPlaceholder(VitaA32::Condition::CC);
			if (positive_no_increment == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitAddImm8(HOST_TMP3, HOST_TMP3, 1))
				return false;

			const size_t positive_done = m_code.EmitBranchPlaceholder();
			if (positive_done == static_cast<size_t>(-1))
				return false;

			const size_t negative_target = m_code.Size();
			if (!m_code.PatchBranch(negative_branch, negative_target, VitaA32::Condition::LT) ||
				!m_code.PatchBranch(positive_no_increment, positive_done, VitaA32::Condition::CC))
			{
				return false;
			}

			if (!m_code.EmitRsbImm32(HOST_TMP0, HOST_TMP3, 0) ||
				!m_code.EmitCmpReg(HOST_TMP4, HOST_TMP0))
			{
				return false;
			}

			const size_t negative_no_increment = m_code.EmitBranchPlaceholder(VitaA32::Condition::LS);
			if (negative_no_increment == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitAddImm8(HOST_TMP3, HOST_TMP3, 1))
				return false;

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(positive_done, done_target) &&
			       m_code.PatchBranch(negative_no_increment, done_target, VitaA32::Condition::LS);
		};

		const auto emit_pmaddw_lower_quirk = [this](unsigned rs_value_reg) {
			if (!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSL, 1) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSR, 1) ||
				!m_code.EmitCmpImm32(HOST_TMP2, 0))
			{
				return false;
			}

			const size_t compare_registers_from_zero = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (compare_registers_from_zero == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP0, 0x7fffffffu) ||
				!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP0))
			{
				return false;
			}

			const size_t no_quirk_from_mask = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_quirk_from_mask == static_cast<size_t>(-1))
				return false;

			const size_t compare_registers_target = m_code.Size();
			if (!m_code.PatchBranch(compare_registers_from_zero, compare_registers_target, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(rs_value_reg, HOST_TMP1))
			{
				return false;
			}

			const size_t no_quirk_from_equal_operands = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (no_quirk_from_equal_operands == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP0, 0x70000000u) ||
				!m_code.EmitAddReg(HOST_TMP4, HOST_TMP4, HOST_TMP0, true) ||
				!m_code.EmitAdcImm8(HOST_TMP3, HOST_TMP3, 0))
			{
				return false;
			}

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(no_quirk_from_mask, done_target, VitaA32::Condition::NE) &&
			       m_code.PatchBranch(no_quirk_from_equal_operands, done_target, VitaA32::Condition::EQ);
		};

		const bool store_rd_from_live_lanes = rd != 0 && rd != rs && rd != rt;
		const auto store_rd_lane_pair = [this, rd, store_rd_from_live_lanes](
											unsigned dest_lane) {
			if (!store_rd_from_live_lanes)
				return true;

			if (dest_lane == 0)
				return EmitStoreGpr64(rd, HOST_TMP2, HOST_TMP3);

			if (!EmitStoreCpuRegsU64(GprOffset(rd) + sizeof(u64),
					HOST_TMP2, HOST_TMP3, HOST_TMP0))
			{
				return false;
			}

			InvalidateGprQCacheForGuest(rd);
			return true;
		};

		const auto emit_lane = [this, rs, rt, subtract, load_word, store_signed_word_as_doubleword,
								   store_rd_lane_pair,
								   emit_divide_signed_64_by_u32_max, emit_pmaddw_lower_quirk](unsigned source_word, unsigned dest_lane) {
			const size_t lo_source_offset = LO_OFFSET + source_word * sizeof(u32);
			const size_t hi_source_offset = HI_OFFSET + source_word * sizeof(u32);
			const size_t lo_dest_offset = LO_OFFSET + dest_lane * sizeof(u64);
			const size_t hi_dest_offset = HI_OFFSET + dest_lane * sizeof(u64);

			if (!load_word(rs, source_word, HOST_TMP0) ||
				(!subtract && source_word == 0 &&
					!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0)) ||
				!load_word(rt, source_word, HOST_TMP1) ||
				!m_code.EmitSmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP2, VitaA32::ShiftType::LSL, 0) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(lo_source_offset)) ||
				!(subtract ? m_code.EmitSubReg(HOST_TMP2, HOST_TMP0, HOST_TMP4) :
							 m_code.EmitAddReg(HOST_TMP2, HOST_TMP0, HOST_TMP4)) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_dest_offset))
			{
				return false;
			}

			if (subtract)
			{
				if (!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP4, 0, true) ||
					!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(hi_source_offset)) ||
					!m_code.EmitSbcReg(HOST_TMP3, HOST_TMP0, HOST_TMP3))
				{
					return false;
				}
			}
			else if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(hi_source_offset)) ||
					 !m_code.EmitAddReg(HOST_TMP3, HOST_TMP3, HOST_TMP0) ||
					 (source_word == 0 && !emit_pmaddw_lower_quirk(HOST_TMP5)))
			{
				return false;
			}

			return emit_divide_signed_64_by_u32_max() &&
			       store_signed_word_as_doubleword(HOST_TMP3, hi_dest_offset) &&
			       store_rd_lane_pair(dest_lane);
		};

		if (!emit_lane(0, 0) || !emit_lane(2, 1))
			return false;

		if (rd == 0 || store_rd_from_live_lanes)
			return true;

		const auto copy_accumulator_to_rd = [this, rd](unsigned rd_word,
												size_t accumulator_offset) {
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS,
					   static_cast<u16>(accumulator_offset)) &&
			       EmitStoreGprWord(rd, rd_word, HOST_TMP0);
		};

		return copy_accumulator_to_rd(0, LO_OFFSET) &&
		       copy_accumulator_to_rd(1, HI_OFFSET) &&
		       copy_accumulator_to_rd(2, LO_OFFSET + sizeof(u64)) &&
		       copy_accumulator_to_rd(3, HI_OFFSET + sizeof(u64));
	}

	bool BlockCompiler::EmitPackedHalfwordMultiplyAccumulate(u32 op, bool subtract)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);

		constexpr unsigned NEON_RS = 0;
		constexpr unsigned NEON_RT = 1;
		constexpr unsigned NEON_PRODUCT_LOW = 2;
		constexpr unsigned NEON_PRODUCT_HIGH = 3;
		constexpr unsigned NEON_LO_PRODUCTS = NEON_PRODUCT_LOW;
		constexpr unsigned NEON_HI_PRODUCTS = NEON_PRODUCT_HIGH;
		constexpr unsigned NEON_LO = 0;
		constexpr unsigned NEON_HI = 1;

		if (rs == 0 || rt == 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuMmiPackedHalfwordMultiplyAccumulateVectorOps++;
#endif
				if (rd == 0)
					return true;

				bool used_qregs[MAX_GPR_QCACHE]{};
				const auto alloc_accumulator_qreg = [&]() {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
						if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
						{
							used_qregs[qreg] = true;
							return qreg;
						}
	}

	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
						if (!used_qregs[qreg])
						{
							used_qregs[qreg] = true;
							return qreg;
						}
	}

					return MAX_GPR_QCACHE;
	};
	const unsigned lo_qreg = alloc_accumulator_qreg();
				const unsigned hi_qreg = alloc_accumulator_qreg();
				if (lo_qreg >= MAX_GPR_QCACHE || hi_qreg >= MAX_GPR_QCACHE)
					return false;
				InvalidateGprQCacheForQreg(lo_qreg);
				InvalidateGprQCacheForQreg(hi_qreg);

				// PCSX2 MMI.cpp::PMADDH()/PMSUBH() add/subtract zero products when
				// either source is $zero; LO/HI remain authoritative and RD receives
				// the same even LO/HI words as the normal path.
				return EmitLoadCpuRegsQ128(LO_OFFSET, lo_qreg, HOST_TMP0) &&
			           EmitLoadCpuRegsQ128(HI_OFFSET, hi_qreg, HOST_TMP1) &&
			           m_code.EmitVtrnI32Q(lo_qreg, hi_qreg) &&
			           EmitStoreGprQ128(rd, lo_qreg, HOST_TMP2);
			}

			const int cached_rs_qreg = FindGprQCache(rs);
			const int cached_rt_qreg = FindGprQCache(rt);
			const auto is_source_qreg = [](int qreg) {
				return qreg >= 0 &&
					   qreg != static_cast<int>(NEON_LO) &&
					   qreg != static_cast<int>(NEON_HI) &&
					   qreg != static_cast<int>(NEON_PRODUCT_LOW) &&
					   qreg != static_cast<int>(NEON_PRODUCT_HIGH);
			};
			bool used_qregs[MAX_GPR_QCACHE]{};
			used_qregs[NEON_LO] = true;
			used_qregs[NEON_HI] = true;
			used_qregs[NEON_PRODUCT_LOW] = true;
			used_qregs[NEON_PRODUCT_HIGH] = true;
			const auto alloc_source_qreg = [&](int cached_qreg) {
				if (is_source_qreg(cached_qreg) &&
					!used_qregs[static_cast<unsigned>(cached_qreg)] &&
	!GprQCacheQregHasFutureQwordReadBeforeWrite(
						static_cast<unsigned>(cached_qreg)))
	{
					const unsigned qreg = static_cast<unsigned>(cached_qreg);
					used_qregs[qreg] = true;
	return qreg;
	}

	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
	{
						used_qregs[qreg] = true;
						return qreg;
	}
	}

	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (!used_qregs[qreg] &&
						!GprQCacheQregHasFutureQwordReadBeforeWrite(qreg))
	{
						used_qregs[qreg] = true;
						return qreg;
	}
	}

	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (!used_qregs[qreg])
	{
						used_qregs[qreg] = true;
						return qreg;
	}
	}

				return MAX_GPR_QCACHE;
			};

			unsigned rs_qreg = alloc_source_qreg(cached_rs_qreg);
			unsigned rt_qreg = rs == rt ? rs_qreg : alloc_source_qreg(cached_rt_qreg);
			if (rs_qreg >= MAX_GPR_QCACHE || rt_qreg >= MAX_GPR_QCACHE)
				return false;
			const bool rt_reuses_rs_qreg = rs == rt;
			if (!(ShouldLoadGprQ128SingleUseEntry(rs) ?
					  EmitLoadGprQ128SingleUseEntry(rs, rs_qreg, HOST_TMP0) :
					  EmitLoadGprQ128(rs, rs_qreg, HOST_TMP0)) ||
				(!rt_reuses_rs_qreg &&
				 !(ShouldLoadGprQ128SingleUseEntry(rt) ?
					   EmitLoadGprQ128SingleUseEntry(rt, rt_qreg, HOST_TMP1) :
					   EmitLoadGprQ128(rt, rt_qreg, HOST_TMP1))))
			{
				return false;
			}

			InvalidateGprQCacheForQreg(NEON_LO);
			InvalidateGprQCacheForQreg(NEON_HI);
			InvalidateGprQCacheForQreg(NEON_PRODUCT_LOW);
			InvalidateGprQCacheForQreg(NEON_PRODUCT_HIGH);
			InvalidateGprQCacheForQreg(NEON_LO_PRODUCTS);
			InvalidateGprQCacheForQreg(NEON_HI_PRODUCTS);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedHalfwordMultiplyAccumulateVectorOps++;
			if (cached_rs_qreg >= 0 || cached_rt_qreg >= 0)
				g_qemuGprQCacheDirectHalfwordAccumulateOps++;
#endif

			// PCSX2 owners: MMI.cpp::PMADDH()/PMSUBH() and
			// x86/iMMI.cpp::recPMSUBH(). Products are signed 16x16->32 lanes;
			// swapping the middle D registers makes LO 0/1/4/5 and HI 2/3/6/7.
			if (!m_code.EmitVmullS16Q(NEON_PRODUCT_LOW, rs_qreg * 2, rt_qreg * 2) ||
				!m_code.EmitVmullS16Q(NEON_PRODUCT_HIGH, rs_qreg * 2 + 1, rt_qreg * 2 + 1) ||
				!m_code.EmitVswpD(NEON_PRODUCT_LOW * 2 + 1, NEON_PRODUCT_HIGH * 2) ||
				!EmitLoadCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) ||
				!EmitLoadCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1))
			{
				return false;
			}

			const bool accumulator_ok = subtract ?
		                                    (m_code.EmitVsubI32Q(NEON_LO, NEON_LO, NEON_LO_PRODUCTS) &&
												m_code.EmitVsubI32Q(NEON_HI, NEON_HI, NEON_HI_PRODUCTS)) :
		                                    (m_code.EmitVaddI32Q(NEON_LO, NEON_LO, NEON_LO_PRODUCTS) &&
												m_code.EmitVaddI32Q(NEON_HI, NEON_HI, NEON_HI_PRODUCTS));
			if (!accumulator_ok ||
				!EmitStoreCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) ||
				!EmitStoreCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1))
			{
				return false;
			}

			if (rd == 0)
				return true;

			return m_code.EmitVtrnI32Q(NEON_LO, NEON_HI) &&
		           EmitStoreGprQ128(rd, NEON_LO, HOST_TMP2);
		}

		bool BlockCompiler::EmitPackedHalfwordPairMultiply(u32 op, bool subtract)
		{
			const unsigned rd = RD(op);
			const unsigned rs = RS(op);
			const unsigned rt = RT(op);

			constexpr unsigned NEON_RS = 0;
			constexpr unsigned NEON_RT = 1;
			constexpr unsigned NEON_PAIR = 2;
			constexpr unsigned NEON_ODD = 3;
			constexpr unsigned NEON_LO = 0;
			constexpr unsigned NEON_HI = 1;

			if (rs == 0 || rt == 0)
			{
				bool used_qregs[MAX_GPR_QCACHE]{};
				const auto alloc_scratch_qreg = [&]() {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
						if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
						{
							used_qregs[qreg] = true;
							return qreg;
						}
	}

	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
						if (!used_qregs[qreg])
						{
							used_qregs[qreg] = true;
							return qreg;
						}
	}

					return MAX_GPR_QCACHE;
	};
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuMmiPackedHalfwordPairMultiplyVectorOps++;
#endif

				// PCSX2 MMI.cpp::PHMADH()/PHMSBH() produce zero pair results with a
				// $zero source; PHMSBH still writes the undocumented ~p(odd) side words.
				if (!subtract)
	{
					const unsigned zero_qreg = alloc_scratch_qreg();
					if (zero_qreg >= MAX_GPR_QCACHE)
						return false;
					InvalidateGprQCacheForQreg(zero_qreg);
					return m_code.EmitVeorQ(zero_qreg, zero_qreg, zero_qreg) &&
				           EmitStoreCpuRegsQ128(LO_OFFSET, zero_qreg, HOST_TMP0) &&
				           EmitStoreCpuRegsQ128(HI_OFFSET, zero_qreg, HOST_TMP1) &&
				           EmitStoreGprQ128(rd, zero_qreg, HOST_TMP2);
	}

				const unsigned pair_qreg = alloc_scratch_qreg();
				const unsigned odd_qreg = alloc_scratch_qreg();
				if (pair_qreg >= MAX_GPR_QCACHE || odd_qreg >= MAX_GPR_QCACHE)
	{
					return false;
	}
				InvalidateGprQCacheForQreg(pair_qreg);
				InvalidateGprQCacheForQreg(odd_qreg);
				if (!m_code.EmitVeorQ(pair_qreg, pair_qreg, pair_qreg) ||
					!m_code.EmitVmvnQ(odd_qreg, pair_qreg))
	{
					return false;
	}

				if (rd == 0)
	{
					return m_code.EmitVtrnI32Q(pair_qreg, odd_qreg) &&
					       EmitStoreCpuRegsQ128(LO_OFFSET, pair_qreg, HOST_TMP0) &&
					       EmitStoreCpuRegsQ128(HI_OFFSET, odd_qreg, HOST_TMP1);
	}

	const unsigned lo_qreg = alloc_scratch_qreg();
				if (lo_qreg >= MAX_GPR_QCACHE)
					return false;
				InvalidateGprQCacheForQreg(lo_qreg);
				if (!m_code.EmitVorrQ(lo_qreg, pair_qreg, pair_qreg) ||
					!m_code.EmitVtrnI32Q(lo_qreg, odd_qreg) ||
					!EmitStoreCpuRegsQ128(LO_OFFSET, lo_qreg, HOST_TMP0) ||
					!EmitStoreCpuRegsQ128(HI_OFFSET, odd_qreg, HOST_TMP1))
	{
					return false;
	}

				return EmitStoreGprQ128(rd, pair_qreg, HOST_TMP2);
			}

			const int cached_rs_qreg = FindGprQCache(rs);
			const int cached_rt_qreg = FindGprQCache(rt);
			const auto is_source_qreg = [](int qreg) {
				return qreg >= 0 &&
					   qreg != static_cast<int>(NEON_LO) &&
					   qreg != static_cast<int>(NEON_HI) &&
					   qreg != static_cast<int>(NEON_PAIR) &&
					   qreg != static_cast<int>(NEON_ODD);
			};
			bool used_qregs[MAX_GPR_QCACHE]{};
			used_qregs[NEON_LO] = true;
			used_qregs[NEON_HI] = true;
			used_qregs[NEON_PAIR] = true;
			used_qregs[NEON_ODD] = true;
			const auto alloc_source_qreg = [&](int cached_qreg) {
				if (is_source_qreg(cached_qreg) &&
					!used_qregs[static_cast<unsigned>(cached_qreg)] &&
	!GprQCacheQregHasFutureQwordReadBeforeWrite(
						static_cast<unsigned>(cached_qreg)))
	{
					const unsigned qreg = static_cast<unsigned>(cached_qreg);
					used_qregs[qreg] = true;
	return qreg;
	}

	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
	{
						used_qregs[qreg] = true;
						return qreg;
	}
	}

	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (!used_qregs[qreg] &&
						!GprQCacheQregHasFutureQwordReadBeforeWrite(qreg))
	{
						used_qregs[qreg] = true;
						return qreg;
	}
	}

	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (!used_qregs[qreg])
	{
						used_qregs[qreg] = true;
						return qreg;
	}
	}

				return MAX_GPR_QCACHE;
			};

			unsigned rs_qreg = alloc_source_qreg(cached_rs_qreg);
			unsigned rt_qreg = rs == rt ? rs_qreg : alloc_source_qreg(cached_rt_qreg);
			if (rs_qreg >= MAX_GPR_QCACHE || rt_qreg >= MAX_GPR_QCACHE)
				return false;
			const bool rt_reuses_rs_qreg = rs == rt;
			if (!(ShouldLoadGprQ128SingleUseEntry(rs) ?
					  EmitLoadGprQ128SingleUseEntry(rs, rs_qreg, HOST_TMP0) :
					  EmitLoadGprQ128(rs, rs_qreg, HOST_TMP0)) ||
				(!rt_reuses_rs_qreg &&
				 !(ShouldLoadGprQ128SingleUseEntry(rt) ?
					   EmitLoadGprQ128SingleUseEntry(rt, rt_qreg, HOST_TMP1) :
					   EmitLoadGprQ128(rt, rt_qreg, HOST_TMP1))))
			{
				return false;
			}

			InvalidateGprQCacheForQreg(NEON_LO);
			InvalidateGprQCacheForQreg(NEON_HI);
			InvalidateGprQCacheForQreg(NEON_PAIR);
			InvalidateGprQCacheForQreg(NEON_ODD);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedHalfwordPairMultiplyVectorOps++;
			if (cached_rs_qreg >= 0 || cached_rt_qreg >= 0)
				g_qemuGprQCacheDirectHalfwordPairMultiplyOps++;
			if (rt_reuses_rs_qreg)
				g_qemuGprQCacheSameSourceQregReuses++;
#endif

			// PCSX2 owners: MMI.cpp::PHMADH()/PHMSBH() and
			// x86/iMMI.cpp::recPHMADH()/recPHMSBH(). The first word in each pair is
			// p(odd) +/- p(even); the second is p(odd), or ~p(odd) for PHMSBH.
			if (!m_code.EmitVmullS16Q(NEON_PAIR, rs_qreg * 2, rt_qreg * 2) ||
				!m_code.EmitVmullS16Q(NEON_ODD, rs_qreg * 2 + 1, rt_qreg * 2 + 1) ||
				!m_code.EmitVuzpI32Q(NEON_PAIR, NEON_ODD))
			{
				return false;
			}

			const bool pair_ok = subtract ?
		                             m_code.EmitVsubI32Q(NEON_PAIR, NEON_ODD, NEON_PAIR) :
		                             m_code.EmitVaddI32Q(NEON_PAIR, NEON_ODD, NEON_PAIR);
			if (!pair_ok)
				return false;

			if (subtract && !m_code.EmitVmvnQ(NEON_ODD, NEON_ODD))
				return false;

			if (rd == 0)
			{
				return m_code.EmitVtrnI32Q(NEON_PAIR, NEON_ODD) &&
				       EmitStoreCpuRegsQ128(LO_OFFSET, NEON_PAIR, HOST_TMP0) &&
				       EmitStoreCpuRegsQ128(HI_OFFSET, NEON_ODD, HOST_TMP1);
			}

			return m_code.EmitVorrQ(NEON_LO, NEON_PAIR, NEON_PAIR) &&
		           m_code.EmitVtrnI32Q(NEON_LO, NEON_ODD) &&
		           EmitStoreCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) &&
		           EmitStoreCpuRegsQ128(HI_OFFSET, NEON_ODD, HOST_TMP1) &&
		           EmitStoreGprQ128(rd, NEON_PAIR, HOST_TMP2);
		}

		bool BlockCompiler::EmitPackedHalfwordMultiply(u32 op)
		{
			const unsigned rd = RD(op);
			const unsigned rs = RS(op);
			const unsigned rt = RT(op);

			constexpr unsigned NEON_RS = 0;
			constexpr unsigned NEON_RT = 1;
			constexpr unsigned NEON_PRODUCT_LOW = 2;
			constexpr unsigned NEON_PRODUCT_HIGH = 3;
			constexpr unsigned NEON_RD = 0;
			constexpr unsigned NEON_LO = NEON_PRODUCT_LOW;
			constexpr unsigned NEON_HI = NEON_PRODUCT_HIGH;

			if (rs == 0 || rt == 0)
			{
				unsigned zero_qreg = NEON_RD;
				for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
				{
					if (!IsGprQCacheQregResident(qreg))
					{
						zero_qreg = qreg;
						break;
					}
				}
				InvalidateGprQCacheForQreg(zero_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuMmiPackedHalfwordMultiplyVectorOps++;
#endif

				// PCSX2 MMI.cpp::PMULTH() multiplies every signed halfword lane, so
				// architectural $zero as either source makes RD/LO/HI all zero.
				return m_code.EmitVeorQ(zero_qreg, zero_qreg, zero_qreg) &&
			           EmitStoreCpuRegsQ128(LO_OFFSET, zero_qreg, HOST_TMP0) &&
			           EmitStoreCpuRegsQ128(HI_OFFSET, zero_qreg, HOST_TMP1) &&
			           EmitStoreGprQ128(rd, zero_qreg, HOST_TMP2);
			}

			const int cached_rs_qreg = FindGprQCache(rs);
			const int cached_rt_qreg = FindGprQCache(rt);
			const auto is_source_qreg = [](int qreg) {
				return qreg >= 0 &&
			           qreg != static_cast<int>(NEON_RD) &&
			           qreg != static_cast<int>(NEON_PRODUCT_LOW) &&
			           qreg != static_cast<int>(NEON_PRODUCT_HIGH);
			};
			bool used_qregs[MAX_GPR_QCACHE]{};
			used_qregs[NEON_RD] = true;
			used_qregs[NEON_PRODUCT_LOW] = true;
			used_qregs[NEON_PRODUCT_HIGH] = true;
			const auto alloc_source_qreg = [&](int cached_qreg) {
				if (is_source_qreg(cached_qreg) &&
					!used_qregs[static_cast<unsigned>(cached_qreg)])
				{
					const unsigned qreg = static_cast<unsigned>(cached_qreg);
					used_qregs[qreg] = true;
					return qreg;
				}

				for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
				{
					if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
					{
						used_qregs[qreg] = true;
						return qreg;
					}
				}

				for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
				{
					if (!used_qregs[qreg] &&
						!GprQCacheQregHasFutureQwordReadBeforeWrite(qreg))
					{
						used_qregs[qreg] = true;
						return qreg;
					}
				}

				for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
				{
					if (!used_qregs[qreg])
					{
						used_qregs[qreg] = true;
						return qreg;
					}
				}

				return MAX_GPR_QCACHE;
			};

			unsigned rs_qreg = alloc_source_qreg(cached_rs_qreg);
			unsigned rt_qreg = rs == rt ? rs_qreg : alloc_source_qreg(cached_rt_qreg);
			if (rs_qreg >= MAX_GPR_QCACHE || rt_qreg >= MAX_GPR_QCACHE)
				return false;
			const bool rt_reuses_rs_qreg = rs == rt;
			if (!(ShouldLoadGprQ128SingleUseEntry(rs) ?
					  EmitLoadGprQ128SingleUseEntry(rs, rs_qreg, HOST_TMP0) :
					  EmitLoadGprQ128(rs, rs_qreg, HOST_TMP0)) ||
				(!rt_reuses_rs_qreg &&
				 !(ShouldLoadGprQ128SingleUseEntry(rt) ?
					   EmitLoadGprQ128SingleUseEntry(rt, rt_qreg, HOST_TMP1) :
					   EmitLoadGprQ128(rt, rt_qreg, HOST_TMP1))))
			{
				return false;
			}

			InvalidateGprQCacheForQreg(NEON_PRODUCT_LOW);
			InvalidateGprQCacheForQreg(NEON_PRODUCT_HIGH);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedHalfwordMultiplyVectorOps++;
			if (cached_rs_qreg >= 0 || cached_rt_qreg >= 0)
				g_qemuGprQCacheDirectHalfwordMultiplyOps++;
#endif

			// PCSX2 owners: MMI.cpp::PMULTH() and x86/iMMI.cpp::recPMULTH().
			// VUZP first packs RD's active products 0/2/4/6; VTRN then restores
			// LO as 0/1/4/5 and HI as 2/3/6/7 without per-D-register copies.
			if (!m_code.EmitVmullS16Q(NEON_PRODUCT_LOW, rs_qreg * 2, rt_qreg * 2) ||
				!m_code.EmitVmullS16Q(NEON_PRODUCT_HIGH, rs_qreg * 2 + 1, rt_qreg * 2 + 1))
			{
				return false;
			}

			if (rd == 0)
			{
				// RD is suppressed, so assemble only PCSX2-visible LO/HI with
				// the same middle-D swap used by PMADDH/PMSUBH.
				return m_code.EmitVswpD(NEON_PRODUCT_LOW * 2 + 1, NEON_PRODUCT_HIGH * 2) &&
			           EmitStoreCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) &&
			           EmitStoreCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1);
			}

			if (!m_code.EmitVuzpI32Q(NEON_LO, NEON_HI))
				return false;

			if (rd != 0 && !m_code.EmitVorrQ(NEON_RD, NEON_LO, NEON_LO))
			{
				return false;
			}

			if (!m_code.EmitVtrnI32Q(NEON_LO, NEON_HI) ||
				!EmitStoreCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) ||
				!EmitStoreCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1))
			{
				return false;
			}

			return EmitStoreGprQ128(rd, NEON_RD, HOST_TMP2);
		}

		bool BlockCompiler::EmitPackedUnsignedWordMultiplyAdd(u32 op)
		{
			const unsigned rd = RD(op);
			const unsigned rs = RS(op);
			const unsigned rt = RT(op);

			constexpr unsigned NEON_RS = 0;
			constexpr unsigned NEON_RT = 1;
			constexpr unsigned NEON_PRODUCT = 2;
			constexpr unsigned NEON_SIGN = 3;

			if (rs == 0 || rt == 0)
			{
				bool used_qregs[MAX_GPR_QCACHE]{};
				const auto alloc_scratch_qreg = [&]() {
					for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
					{
						if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
						{
							used_qregs[qreg] = true;
							return qreg;
						}
					}

					for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
					{
						if (!used_qregs[qreg])
						{
							used_qregs[qreg] = true;
							return qreg;
						}
					}

					return MAX_GPR_QCACHE;
				};
				const unsigned acc_qreg = alloc_scratch_qreg();
				const unsigned sign_qreg = alloc_scratch_qreg();
				const unsigned product_qreg = alloc_scratch_qreg();
				if (acc_qreg >= MAX_GPR_QCACHE || sign_qreg >= MAX_GPR_QCACHE ||
					product_qreg >= MAX_GPR_QCACHE)
				{
					return false;
				}
				InvalidateGprQCacheForQreg(acc_qreg);
				InvalidateGprQCacheForQreg(sign_qreg);
				InvalidateGprQCacheForQreg(product_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedWordMultiplyAddVectorOps++;
#endif

			// PCSX2 MMI.cpp::_PMADDUW() adds a zero product to the raw
			// LO.UL[0/2]|HI.UL[0/2] accumulators when either source is $zero.
			if (!EmitLoadCpuRegsQ128(LO_OFFSET, acc_qreg, HOST_TMP0) ||
				!EmitLoadCpuRegsQ128(HI_OFFSET, sign_qreg, HOST_TMP1) ||
				!m_code.EmitVtrnI32Q(acc_qreg, sign_qreg) ||
				!m_code.EmitVorrQ(product_qreg, acc_qreg, acc_qreg) ||
				!m_code.EmitVshrS32Q(sign_qreg, product_qreg, 31) ||
				!m_code.EmitVtrnI32Q(product_qreg, sign_qreg) ||
				!EmitStoreCpuRegsQ128(LO_OFFSET, product_qreg, HOST_TMP0) ||
				!m_code.EmitVextI8Q(product_qreg, acc_qreg, acc_qreg, 4) ||
				!m_code.EmitVshrS32Q(sign_qreg, product_qreg, 31) ||
				!m_code.EmitVtrnI32Q(product_qreg, sign_qreg) ||
				!EmitStoreCpuRegsQ128(HI_OFFSET, product_qreg, HOST_TMP1))
			{
				return false;
			}

			return EmitStoreGprQ128(rd, acc_qreg, HOST_TMP2);
		}

		const int cached_rs_qreg = FindGprQCache(rs);
		const int cached_rt_qreg = FindGprQCache(rt);
		const auto is_source_qreg = [](int qreg) {
			return qreg >= 0 &&
				   qreg != static_cast<int>(NEON_PRODUCT) &&
				   qreg != static_cast<int>(NEON_SIGN);
		};
		bool used_qregs[MAX_GPR_QCACHE]{};
		used_qregs[NEON_PRODUCT] = true;
		used_qregs[NEON_SIGN] = true;
		const auto alloc_source_qreg = [&](int cached_qreg) {
			if (is_source_qreg(cached_qreg) &&
				!used_qregs[static_cast<unsigned>(cached_qreg)] &&
	!GprQCacheQregHasFutureQwordReadBeforeWrite(
					static_cast<unsigned>(cached_qreg)))
			{
				const unsigned qreg = static_cast<unsigned>(cached_qreg);
				used_qregs[qreg] = true;
	return qreg;
			}

			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
	{
					used_qregs[qreg] = true;
	return qreg;
	}
			}

			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg] &&
	!GprQCacheQregHasFutureQwordReadBeforeWrite(qreg))
	{
					used_qregs[qreg] = true;
	return qreg;
	}
			}

			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg])
	{
					used_qregs[qreg] = true;
	return qreg;
	}
			}

			return MAX_GPR_QCACHE;
		};

		unsigned rs_qreg = alloc_source_qreg(cached_rs_qreg);
		unsigned rt_qreg = MAX_GPR_QCACHE;

		const bool rt_reuses_rs_qreg = rs == rt;
		if (rt_reuses_rs_qreg)
		{
			rt_qreg = rs_qreg;
		}
		else
		{
			rt_qreg = alloc_source_qreg(cached_rt_qreg);
		}
		if (rs_qreg >= MAX_GPR_QCACHE || rt_qreg >= MAX_GPR_QCACHE)
			return false;
		if (!(ShouldLoadGprQ128SingleUseEntry(rs) ?
				  EmitLoadGprQ128SingleUseEntry(rs, rs_qreg, HOST_TMP0) :
				  EmitLoadGprQ128(rs, rs_qreg, HOST_TMP0)) ||
			(!rt_reuses_rs_qreg &&
			 !(ShouldLoadGprQ128SingleUseEntry(rt) ?
				   EmitLoadGprQ128SingleUseEntry(rt, rt_qreg, HOST_TMP1) :
				   EmitLoadGprQ128(rt, rt_qreg, HOST_TMP1))))
		{
			return false;
		}

		InvalidateGprQCacheForQreg(rs_qreg);
		InvalidateGprQCacheForQreg(rt_qreg);
		InvalidateGprQCacheForQreg(NEON_PRODUCT);
		InvalidateGprQCacheForQreg(NEON_SIGN);
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuMmiPackedWordMultiplyAddVectorOps++;
		if (is_source_qreg(cached_rs_qreg) || is_source_qreg(cached_rt_qreg))
			g_qemuGprQCacheDirectWordMultiplyAddOps++;
		if (rt_reuses_rs_qreg)
			g_qemuGprQCacheSameSourceQregReuses++;
#endif

		// PCSX2 owners: MMI.cpp::_PMADDUW() and x86/iMMI.cpp::recPMADDUW().
		// LO.UL[0/2] and HI.UL[0/2] form raw 64-bit accumulators; LO/HI are
		// written back as sign-extended low/high 32-bit result halves. As with
		// PMULTW/PMULTUW, VTRN.32 on each source D pair forms active words 0/2.
		const unsigned acc_lo_qreg = rs_qreg;
		const unsigned acc_hi_qreg = rt_reuses_rs_qreg ? NEON_SIGN : rt_qreg;
		if (!m_code.EmitVtrnI32D(rs_qreg * 2, rs_qreg * 2 + 1) ||
			(!rt_reuses_rs_qreg &&
				!m_code.EmitVtrnI32D(rt_qreg * 2, rt_qreg * 2 + 1)) ||
			!m_code.EmitVmullU32Q(NEON_PRODUCT, rs_qreg * 2, rt_qreg * 2) ||
			!EmitLoadCpuRegsQ128(LO_OFFSET, acc_lo_qreg, HOST_TMP0) ||
			!EmitLoadCpuRegsQ128(HI_OFFSET, acc_hi_qreg, HOST_TMP1) ||
			!m_code.EmitVtrnI32Q(acc_lo_qreg, acc_hi_qreg) ||
			!m_code.EmitVaddI64Q(NEON_PRODUCT, NEON_PRODUCT, acc_lo_qreg))
		{
			return false;
		}

		if (rd == 0)
		{
			// PCSX2 MMI.cpp::PMADDUW() writes only LO/HI when RD is $zero; split
			// the accumulated product in place instead of keeping a pristine RD copy.
			return m_code.EmitVextI8Q(acc_lo_qreg, NEON_PRODUCT, NEON_PRODUCT, 4) &&
			       m_code.EmitVshrS32Q(NEON_SIGN, NEON_PRODUCT, 31) &&
			       m_code.EmitVtrnI32Q(NEON_PRODUCT, NEON_SIGN) &&
			       EmitStoreCpuRegsQ128(LO_OFFSET, NEON_PRODUCT, HOST_TMP0) &&
			       m_code.EmitVshrS32Q(NEON_SIGN, acc_lo_qreg, 31) &&
			       m_code.EmitVtrnI32Q(acc_lo_qreg, NEON_SIGN) &&
			       EmitStoreCpuRegsQ128(HI_OFFSET, acc_lo_qreg, HOST_TMP1);
		}

		if (!m_code.EmitVorrQ(acc_lo_qreg, NEON_PRODUCT, NEON_PRODUCT) ||
			!m_code.EmitVshrS32Q(NEON_SIGN, acc_lo_qreg, 31) ||
			!m_code.EmitVtrnI32Q(acc_lo_qreg, NEON_SIGN) ||
			!EmitStoreCpuRegsQ128(LO_OFFSET, acc_lo_qreg, HOST_TMP0) ||
			!m_code.EmitVextI8Q(acc_lo_qreg, NEON_PRODUCT, NEON_PRODUCT, 4) ||
			!m_code.EmitVshrS32Q(NEON_SIGN, acc_lo_qreg, 31) ||
			!m_code.EmitVtrnI32Q(acc_lo_qreg, NEON_SIGN) ||
			!EmitStoreCpuRegsQ128(HI_OFFSET, acc_lo_qreg, HOST_TMP1))
		{
			return false;
		}

		return EmitStoreGprQ128(rd, NEON_PRODUCT, HOST_TMP2);
	}

	bool BlockCompiler::EmitMmiVectorOp(u32 op, MmiVectorOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RS = 0;
		constexpr unsigned NEON_RT = 1;
		constexpr unsigned NEON_RD = 2;

		const auto emit_store_result = [this, rd](unsigned qreg) {
			return EmitStoreGprQ128(rd, qreg, HOST_TMP2);
		};

		const auto choose_folded_result_qreg = [&](int avoid0 = -1) {
			const u32 avoid_mask = avoid0 >= 0 ? (1u << static_cast<unsigned>(avoid0)) : 0;
			const unsigned qreg = SelectGprQCacheScratchQreg(avoid_mask);
			return qreg < MAX_GPR_QCACHE ? qreg : NEON_RD;
		};

		const auto emit_zero_result = [&]() {
			const unsigned result_qreg = choose_folded_result_qreg();
			InvalidateGprQCacheForQreg(result_qreg);
			return m_code.EmitVeorQ(result_qreg, result_qreg, result_qreg) &&
			       emit_store_result(result_qreg);
		};

		const auto emit_copy_result = [&](unsigned guest_reg) {
			if (guest_reg == 0)
				return emit_zero_result();

			if (guest_reg == rd)
				return true;

			const int cached_qreg = FindGprQCache(guest_reg);
			if (cached_qreg >= 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectCopyStores++;
#endif
				return EmitStoreGprQ128PreservingCachedSourceIfFutureRead(rd, guest_reg,
					static_cast<unsigned>(cached_qreg), HOST_TMP2, nullptr);
			}

			const unsigned result_qreg = choose_folded_result_qreg();
			return EmitLoadGprQ128(guest_reg, result_qreg, HOST_TMP0) &&
			       emit_store_result(result_qreg);
		};

		const auto emit_inverted_result = [&](unsigned guest_reg) {
			unsigned result_qreg = choose_folded_result_qreg();
			if (guest_reg == 0)
			{
				InvalidateGprQCacheForQreg(result_qreg);
				if (!m_code.EmitVeorQ(result_qreg, result_qreg, result_qreg))
					return false;
			}
			else
			{
				const int cached_qreg = FindGprQCache(guest_reg);
				if (cached_qreg >= 0)
				{
					result_qreg = choose_folded_result_qreg(cached_qreg);
					InvalidateGprQCacheForQreg(result_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuGprQCacheDirectInvertStores++;
#endif
					return m_code.EmitVmvnQ(result_qreg, static_cast<unsigned>(cached_qreg)) &&
					       emit_store_result(result_qreg);
				}

				if (!EmitLoadGprQ128(guest_reg, result_qreg, HOST_TMP0))
					return false;
				InvalidateGprQCacheForGuest(guest_reg);
			}

			return m_code.EmitVmvnQ(result_qreg, result_qreg) &&
			       emit_store_result(result_qreg);
		};

		// PCSX2 owners: MMI.cpp packed add/sub/compare/min/max/logical helpers.
		// Fold exact zero/idempotent cases before the generic two-source NEON path.
		switch (operation)
		{
			case MmiVectorOp::AddWord:
			case MmiVectorOp::AddHalfword:
			case MmiVectorOp::AddByte:
			case MmiVectorOp::SaturatingAddSignedWord:
			case MmiVectorOp::SaturatingAddSignedHalfword:
			case MmiVectorOp::SaturatingAddSignedByte:
			case MmiVectorOp::SaturatingAddUnsignedWord:
			case MmiVectorOp::SaturatingAddUnsignedHalfword:
			case MmiVectorOp::SaturatingAddUnsignedByte:
				if (rs == 0)
					return emit_copy_result(rt);
				if (rt == 0)
					return emit_copy_result(rs);
				break;
			case MmiVectorOp::SubtractWord:
			case MmiVectorOp::SubtractHalfword:
			case MmiVectorOp::SubtractByte:
			case MmiVectorOp::SaturatingSubtractSignedWord:
			case MmiVectorOp::SaturatingSubtractSignedHalfword:
			case MmiVectorOp::SaturatingSubtractSignedByte:
				if (rs == rt)
					return emit_zero_result();
				if (rt == 0)
					return emit_copy_result(rs);
				break;
			case MmiVectorOp::SaturatingSubtractUnsignedWord:
			case MmiVectorOp::SaturatingSubtractUnsignedHalfword:
			case MmiVectorOp::SaturatingSubtractUnsignedByte:
				if (rs == 0 || rs == rt)
					return emit_zero_result();
				if (rt == 0)
					return emit_copy_result(rs);
				break;
			case MmiVectorOp::CompareGreaterSignedWord:
			case MmiVectorOp::CompareGreaterSignedHalfword:
			case MmiVectorOp::CompareGreaterSignedByte:
				if (rs == rt)
					return emit_zero_result();
				break;
			case MmiVectorOp::CompareEqualWord:
			case MmiVectorOp::CompareEqualHalfword:
			case MmiVectorOp::CompareEqualByte:
				if (rs == rt)
					return emit_inverted_result(0);
				break;
			case MmiVectorOp::MaxSignedWord:
			case MmiVectorOp::MaxSignedHalfword:
			case MmiVectorOp::MinSignedWord:
			case MmiVectorOp::MinSignedHalfword:
				if (rs == rt)
					return emit_copy_result(rs);
				break;
			case MmiVectorOp::BitwiseAnd:
				if (rs == 0 || rt == 0)
					return emit_zero_result();
				if (rs == rt)
					return emit_copy_result(rs);
				break;
			case MmiVectorOp::BitwiseXor:
				if (rs == rt)
					return emit_zero_result();
				if (rs == 0)
					return emit_copy_result(rt);
				if (rt == 0)
					return emit_copy_result(rs);
				break;
			case MmiVectorOp::BitwiseOr:
				if (rs == rt)
					return emit_copy_result(rs);
				if (rs == 0)
					return emit_copy_result(rt);
				if (rt == 0)
					return emit_copy_result(rs);
				break;
			case MmiVectorOp::BitwiseNor:
				if (rs == rt)
					return emit_inverted_result(rs);
				if (rs == 0)
					return emit_inverted_result(rt);
				if (rt == 0)
					return emit_inverted_result(rs);
				break;
			default:
				break;
		}

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			return EmitCpuRegsAddress(host_reg, GprOffset(guest_reg));
		};

		const auto emit_store_rd = [&](unsigned qreg) {
			if (rd == rs && rs != 0)
			{
				return emit_gpr_address(rd, HOST_TMP0) &&
				       EmitStoreGprQ128ToAddress(rd, qreg, HOST_TMP0);
			}

			if (rd == rt && rt != 0)
			{
				return emit_gpr_address(rd, HOST_TMP1) &&
				       EmitStoreGprQ128ToAddress(rd, qreg, HOST_TMP1);
			}

			return EmitStoreGprQ128(rd, qreg, HOST_TMP2);
		};

		const auto emit_vector_op = [&](unsigned result_qreg, unsigned rs_qreg, unsigned rt_qreg) {
			switch (operation)
			{
				case MmiVectorOp::AddWord:
					return m_code.EmitVaddI32Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SubtractWord:
					return m_code.EmitVsubI32Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::CompareGreaterSignedWord:
					return m_code.EmitVcgtS32Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::MaxSignedWord:
					return m_code.EmitVmaxS32Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::AddHalfword:
					return m_code.EmitVaddI16Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SubtractHalfword:
					return m_code.EmitVsubI16Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::CompareGreaterSignedHalfword:
					return m_code.EmitVcgtS16Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::MaxSignedHalfword:
					return m_code.EmitVmaxS16Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::AddByte:
					return m_code.EmitVaddI8Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SubtractByte:
					return m_code.EmitVsubI8Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::CompareGreaterSignedByte:
					return m_code.EmitVcgtS8Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::CompareEqualWord:
					return m_code.EmitVceqI32Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::MinSignedWord:
					return m_code.EmitVminS32Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::CompareEqualHalfword:
					return m_code.EmitVceqI16Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::MinSignedHalfword:
					return m_code.EmitVminS16Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::CompareEqualByte:
					return m_code.EmitVceqI8Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SaturatingAddSignedWord:
					return m_code.EmitVqaddS32Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SaturatingSubtractSignedWord:
					return m_code.EmitVqsubS32Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SaturatingAddSignedHalfword:
					return m_code.EmitVqaddS16Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SaturatingSubtractSignedHalfword:
					return m_code.EmitVqsubS16Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SaturatingAddSignedByte:
					return m_code.EmitVqaddS8Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SaturatingSubtractSignedByte:
					return m_code.EmitVqsubS8Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SaturatingAddUnsignedWord:
					return m_code.EmitVqaddU32Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SaturatingSubtractUnsignedWord:
					return m_code.EmitVqsubU32Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SaturatingAddUnsignedHalfword:
					return m_code.EmitVqaddU16Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SaturatingSubtractUnsignedHalfword:
					return m_code.EmitVqsubU16Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SaturatingAddUnsignedByte:
					return m_code.EmitVqaddU8Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::SaturatingSubtractUnsignedByte:
					return m_code.EmitVqsubU8Q(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::BitwiseAnd:
					return m_code.EmitVandQ(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::BitwiseXor:
					return m_code.EmitVeorQ(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::BitwiseOr:
					return m_code.EmitVorrQ(result_qreg, rs_qreg, rt_qreg);
				case MmiVectorOp::BitwiseNor:
					return m_code.EmitVorrQ(result_qreg, rs_qreg, rt_qreg) &&
					       m_code.EmitVmvnQ(result_qreg, result_qreg);
			}

			return false;
		};

		const int cached_rs_qreg = FindGprQCache(rs);
		const int cached_rt_qreg = FindGprQCache(rt);
		const auto should_load_single_use_entry_operand = [&](unsigned guest_reg) {
			return ShouldLoadGprQ128SingleUseEntry(guest_reg);
		};
		const auto load_qword_operand = [&](unsigned guest_reg, unsigned qreg,
										 unsigned address_scratch) {
			if (should_load_single_use_entry_operand(guest_reg))
				return EmitLoadGprQ128SingleUseEntry(guest_reg, qreg,
					address_scratch);
			return EmitLoadGprQ128(guest_reg, qreg, address_scratch);
		};
		const auto choose_result_qreg = [&](int rs_qreg, int rt_qreg) {
			if (rd == rs && rs != 0 && rs_qreg >= 0)
				return static_cast<unsigned>(rs_qreg);
			if (rd == rt && rt != 0 && rt_qreg >= 0)
				return static_cast<unsigned>(rt_qreg);

			u32 avoid_mask = 0;
			if (rs_qreg >= 0)
				avoid_mask |= 1u << static_cast<unsigned>(rs_qreg);
			if (rt_qreg >= 0)
				avoid_mask |= 1u << static_cast<unsigned>(rt_qreg);
			const unsigned qreg = SelectGprQCacheScratchQreg(avoid_mask);
			return qreg < MAX_GPR_QCACHE ? qreg : NEON_RD;
		};
		if (rs == rt && rs != 0)
		{
			const unsigned source_qreg = cached_rs_qreg >= 0 ?
			                                 static_cast<unsigned>(cached_rs_qreg) :
			                                 choose_folded_result_qreg();
			if (cached_rs_qreg < 0 &&
				!load_qword_operand(rs, source_qreg, HOST_TMP0))
			{
				return false;
			}

			const unsigned result_qreg =
				choose_result_qreg(static_cast<int>(source_qreg),
					static_cast<int>(source_qreg));
			const bool in_place = result_qreg == source_qreg;
			if (in_place)
				InvalidateGprQCacheForGuest(rs);
			else
				InvalidateGprQCacheForQreg(result_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheDirectBinaryOps++;
			g_qemuGprQCacheSameSourceQregReuses++;
			if (in_place)
				g_qemuGprQCacheDirectInPlaceBinaryOps++;
#endif
			return emit_vector_op(result_qreg, source_qreg, source_qreg) &&
			       emit_store_rd(result_qreg);
		}
		if (cached_rs_qreg >= 0 && cached_rt_qreg >= 0)
		{
			const unsigned result_qreg =
				choose_result_qreg(cached_rs_qreg, cached_rt_qreg);
			const bool in_place =
				result_qreg == static_cast<unsigned>(cached_rs_qreg) ||
				result_qreg == static_cast<unsigned>(cached_rt_qreg);
			if (!in_place)
				InvalidateGprQCacheForQreg(result_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheDirectBinaryOps++;
			if (in_place)
				g_qemuGprQCacheDirectInPlaceBinaryOps++;
#endif
			return emit_vector_op(result_qreg, static_cast<unsigned>(cached_rs_qreg),
					   static_cast<unsigned>(cached_rt_qreg)) &&
			       emit_store_rd(result_qreg);
		}

		const auto emit_cached_zero_binary_op =
			[&](unsigned cached_qreg, bool cached_is_rs) {
				const unsigned result_qreg =
					choose_result_qreg(static_cast<int>(cached_qreg), -1);
				const bool in_place = result_qreg == cached_qreg;
				const bool zero_reuses_result = !in_place;
				const auto choose_zero_qreg = [&](unsigned avoid0, unsigned avoid1) {
					const unsigned qreg = SelectGprQCacheScratchQreg(
						(1u << avoid0) | (1u << avoid1));
					return qreg < MAX_GPR_QCACHE ? qreg : NEON_RS;
				};
				const unsigned zero_qreg = zero_reuses_result ?
			                                   result_qreg :
			                                   choose_zero_qreg(cached_qreg, result_qreg);
				InvalidateGprQCacheForQreg(result_qreg);
				if (!zero_reuses_result)
					InvalidateGprQCacheForQreg(zero_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectZeroBinaryOps++;
				if (zero_reuses_result)
					g_qemuGprQCacheDirectZeroResultReuses++;
				if (in_place)
					g_qemuGprQCacheDirectInPlaceBinaryOps++;
#endif
				return m_code.EmitVeorQ(zero_qreg, zero_qreg, zero_qreg) &&
			           (cached_is_rs ?
							   emit_vector_op(result_qreg, cached_qreg, zero_qreg) :
							   emit_vector_op(result_qreg, zero_qreg, cached_qreg)) &&
			           emit_store_rd(result_qreg);
			};

		if (cached_rs_qreg >= 0 && rt == 0)
			return emit_cached_zero_binary_op(
				static_cast<unsigned>(cached_rs_qreg), true);

		if (cached_rt_qreg >= 0 && rs == 0)
			return emit_cached_zero_binary_op(
				static_cast<unsigned>(cached_rt_qreg), false);

		if (cached_rs_qreg >= 0 && rt != 0)
		{
			const unsigned rs_qreg = static_cast<unsigned>(cached_rs_qreg);
			const auto choose_operand_qreg = [&](unsigned avoid) {
				const unsigned qreg = SelectGprQCacheScratchQreg(1u << avoid);
				return qreg < MAX_GPR_QCACHE ? qreg :
					(avoid == NEON_RT ? NEON_RS : NEON_RT);
			};
			const unsigned rt_qreg = choose_operand_qreg(rs_qreg);
			const unsigned result_qreg =
				choose_result_qreg(static_cast<int>(rs_qreg), static_cast<int>(rt_qreg));
			const bool in_place = result_qreg == rs_qreg || result_qreg == rt_qreg;
			if (!load_qword_operand(rt, rt_qreg, HOST_TMP1))
				return false;
			if (!in_place)
				InvalidateGprQCacheForQreg(result_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheDirectMixedBinaryOps++;
			if (in_place)
				g_qemuGprQCacheDirectInPlaceBinaryOps++;
#endif
			return emit_vector_op(result_qreg, rs_qreg, rt_qreg) &&
			       emit_store_rd(result_qreg);
		}

		if (cached_rt_qreg >= 0 && rs != 0)
		{
			const unsigned rt_qreg = static_cast<unsigned>(cached_rt_qreg);
			const auto choose_operand_qreg = [&](unsigned avoid) {
				const unsigned qreg = SelectGprQCacheScratchQreg(1u << avoid);
				return qreg < MAX_GPR_QCACHE ? qreg :
					(avoid == NEON_RS ? NEON_RT : NEON_RS);
			};
			const unsigned rs_qreg = choose_operand_qreg(rt_qreg);
			const unsigned result_qreg =
				choose_result_qreg(static_cast<int>(rs_qreg), static_cast<int>(rt_qreg));
			const bool in_place = result_qreg == rs_qreg || result_qreg == rt_qreg;
			if (!load_qword_operand(rs, rs_qreg, HOST_TMP0))
				return false;
			if (!in_place)
				InvalidateGprQCacheForQreg(result_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheDirectMixedBinaryOps++;
			if (in_place)
				g_qemuGprQCacheDirectInPlaceBinaryOps++;
#endif
			return emit_vector_op(result_qreg, rs_qreg, rt_qreg) &&
			       emit_store_rd(result_qreg);
		}

		u32 used_qreg_mask = 0;
		const auto choose_qreg = [&]() {
			const unsigned qreg = SelectGprQCacheScratchQreg(used_qreg_mask);
			if (qreg < MAX_GPR_QCACHE)
				used_qreg_mask |= 1u << qreg;
			return qreg;
		};
		const unsigned rs_qreg = choose_qreg();
		const unsigned rt_qreg = choose_qreg();
		const unsigned result_qreg =
			(rd == rs && rs != 0) ? rs_qreg :
			(rd == rt && rt != 0) ? rt_qreg :
									choose_qreg();
		if (rs_qreg >= MAX_GPR_QCACHE || rt_qreg >= MAX_GPR_QCACHE ||
			result_qreg >= MAX_GPR_QCACHE ||
			!load_qword_operand(rs, rs_qreg, HOST_TMP0) ||
			!load_qword_operand(rt, rt_qreg, HOST_TMP1))
		{
			return false;
		}

		const bool in_place = result_qreg == rs_qreg || result_qreg == rt_qreg;
		if (!in_place)
			InvalidateGprQCacheForQreg(result_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprQCacheDirectBinaryOps++;
		if (in_place)
			g_qemuGprQCacheDirectInPlaceBinaryOps++;
#endif
		if (!emit_vector_op(result_qreg, rs_qreg, rt_qreg) ||
			!emit_store_rd(result_qreg))
		{
			return false;
		}

		return true;
	}

	bool BlockCompiler::EmitMmiUnaryRtVectorOp(u32 op, MmiUnaryVectorOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RT = 0;
		const auto choose_nonresident_qreg = [&]() {
			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!IsGprQCacheQregResident(qreg))
	return qreg;
			}

			return NEON_RT;
		};
		if (rt == 0)
		{
			const unsigned result_qreg = choose_nonresident_qreg();
			InvalidateGprQCacheForQreg(result_qreg);
			return m_code.EmitVeorQ(result_qreg, result_qreg, result_qreg) &&
				   EmitStoreGprQ128(rd, result_qreg, HOST_TMP2);
		}

		const int cached_qreg = FindGprQCache(rt);
		if (cached_qreg >= 0)
		{
			const unsigned result_qreg = static_cast<unsigned>(cached_qreg);
			if (rd != rt &&
				!PreserveGprQCacheGuestForFutureRead(rt, result_qreg,
					result_qreg, MAX_GPR_QCACHE, nullptr))
			{
				return false;
			}
			InvalidateGprQCacheForQreg(result_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheDirectTransformStores++;
#endif
			bool vector_op_ok = false;
			switch (operation)
			{
				case MmiUnaryVectorOp::AbsoluteSignedWord:
					vector_op_ok = m_code.EmitVqabsS32Q(result_qreg, result_qreg);
					break;
				case MmiUnaryVectorOp::AbsoluteSignedHalfword:
					vector_op_ok = m_code.EmitVqabsS16Q(result_qreg, result_qreg);
					break;
			}

			return vector_op_ok &&
				   EmitStoreGprQ128(rd, result_qreg, HOST_TMP2);
		}

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			return EmitCpuRegsAddress(host_reg, GprOffset(guest_reg));
		};

		const unsigned rt_qreg = choose_nonresident_qreg();
		const auto emit_store_rd = [&]() {
			if (rd == rt && rt != 0)
			{
				return emit_gpr_address(rd, HOST_TMP0) &&
					   EmitStoreGprQ128ToAddress(rd, rt_qreg, HOST_TMP0);
			}

			return EmitStoreGprQ128(rd, rt_qreg, HOST_TMP2);
		};

		const bool single_use_rt =
			ShouldLoadGprQ128SingleUseEntry(rt);
		if (!(single_use_rt ?
				  EmitLoadGprQ128SingleUseEntry(rt, rt_qreg, HOST_TMP0) :
				  EmitLoadGprQ128(rt, rt_qreg, HOST_TMP0)))
		{
			return false;
		}

		bool vector_op_ok = false;
		switch (operation)
		{
			case MmiUnaryVectorOp::AbsoluteSignedWord:
				vector_op_ok = m_code.EmitVqabsS32Q(rt_qreg, rt_qreg);
				break;
			case MmiUnaryVectorOp::AbsoluteSignedHalfword:
				vector_op_ok = m_code.EmitVqabsS16Q(rt_qreg, rt_qreg);
				break;
		}

		if (!vector_op_ok ||
			!emit_store_rd())
		{
			return false;
		}

		return true;
	}

	bool BlockCompiler::EmitMmiImmediateShiftOp(u32 op, MmiImmediateShiftOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RT = 0;
		const auto choose_nonresident_qreg = [&]() {
			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!IsGprQCacheQregResident(qreg))
	return qreg;
			}

			return NEON_RT;
		};

		const u8 amount = static_cast<u8>((operation == MmiImmediateShiftOp::ShiftLeftHalfword ||
			operation == MmiImmediateShiftOp::ShiftRightLogicalHalfword ||
			operation == MmiImmediateShiftOp::ShiftRightArithmeticHalfword) ?
			(SA(op) & 0x0f) : SA(op));
		if (amount == 0 && rd == rt && rt != 0)
			return true;
		if (rt == 0)
		{
			const unsigned result_qreg = choose_nonresident_qreg();
			InvalidateGprQCacheForQreg(result_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiImmediateShiftZeroSourceOps++;
#endif
			return m_code.EmitVeorQ(result_qreg, result_qreg, result_qreg) &&
				   EmitStoreGprQ128(rd, result_qreg, HOST_TMP2);
		}
		if (amount == 0)
		{
			const int cached_qreg = FindGprQCache(rt);
			if (cached_qreg >= 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectCopyStores++;
#endif
				return EmitStoreGprQ128PreservingCachedSourceIfFutureRead(rd, rt,
					static_cast<unsigned>(cached_qreg), HOST_TMP2, nullptr);
			}
		}
		else
		{
			const int cached_qreg = FindGprQCache(rt);
			if (cached_qreg >= 0)
			{
				const unsigned result_qreg = static_cast<unsigned>(cached_qreg);
				if (rd != rt &&
					!PreserveGprQCacheGuestForFutureRead(rt, result_qreg,
						result_qreg, MAX_GPR_QCACHE, nullptr))
				{
					return false;
				}
				InvalidateGprQCacheForQreg(result_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectTransformStores++;
#endif
				bool vector_op_ok = false;
				switch (operation)
	{
					case MmiImmediateShiftOp::ShiftLeftHalfword:
						vector_op_ok = m_code.EmitVshlI16Q(result_qreg, result_qreg, amount);
						break;
					case MmiImmediateShiftOp::ShiftRightLogicalHalfword:
						vector_op_ok = m_code.EmitVshrU16Q(result_qreg, result_qreg, amount);
						break;
					case MmiImmediateShiftOp::ShiftRightArithmeticHalfword:
						vector_op_ok = m_code.EmitVshrS16Q(result_qreg, result_qreg, amount);
						break;
					case MmiImmediateShiftOp::ShiftLeftWord:
						vector_op_ok = m_code.EmitVshlI32Q(result_qreg, result_qreg, amount);
						break;
					case MmiImmediateShiftOp::ShiftRightLogicalWord:
						vector_op_ok = m_code.EmitVshrU32Q(result_qreg, result_qreg, amount);
						break;
					case MmiImmediateShiftOp::ShiftRightArithmeticWord:
						vector_op_ok = m_code.EmitVshrS32Q(result_qreg, result_qreg, amount);
						break;
	}

				return vector_op_ok &&
					   EmitStoreGprQ128(rd, result_qreg, HOST_TMP2);
			}
		}

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			return EmitCpuRegsAddress(host_reg, GprOffset(guest_reg));
		};

		const unsigned rt_qreg = choose_nonresident_qreg();
		const auto emit_store_rd = [&]() {
			if (rd == rt && rt != 0)
			{
				return emit_gpr_address(rd, HOST_TMP0) &&
					   EmitStoreGprQ128ToAddress(rd, rt_qreg, HOST_TMP0);
			}

			return EmitStoreGprQ128(rd, rt_qreg, HOST_TMP2);
		};

		const bool single_use_rt =
			ShouldLoadGprQ128SingleUseEntry(rt);
		if (!(single_use_rt ?
				  EmitLoadGprQ128SingleUseEntry(rt, rt_qreg, HOST_TMP0) :
				  EmitLoadGprQ128(rt, rt_qreg, HOST_TMP0)))
		{
			return false;
		}

		bool vector_op_ok = true;
		if (amount != 0)
		{
			switch (operation)
			{
				case MmiImmediateShiftOp::ShiftLeftHalfword:
					vector_op_ok = m_code.EmitVshlI16Q(rt_qreg, rt_qreg, amount);
					break;
				case MmiImmediateShiftOp::ShiftRightLogicalHalfword:
					vector_op_ok = m_code.EmitVshrU16Q(rt_qreg, rt_qreg, amount);
					break;
				case MmiImmediateShiftOp::ShiftRightArithmeticHalfword:
					vector_op_ok = m_code.EmitVshrS16Q(rt_qreg, rt_qreg, amount);
					break;
				case MmiImmediateShiftOp::ShiftLeftWord:
					vector_op_ok = m_code.EmitVshlI32Q(rt_qreg, rt_qreg, amount);
					break;
				case MmiImmediateShiftOp::ShiftRightLogicalWord:
					vector_op_ok = m_code.EmitVshrU32Q(rt_qreg, rt_qreg, amount);
					break;
				case MmiImmediateShiftOp::ShiftRightArithmeticWord:
					vector_op_ok = m_code.EmitVshrS32Q(rt_qreg, rt_qreg, amount);
					break;
			}
		}

		if (!vector_op_ok ||
			!emit_store_rd())
		{
			return false;
		}

		return true;
	}

	bool BlockCompiler::EmitMmiVariableWordShiftOp(u32 op, MmiVariableWordShiftOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RT = 0;
		constexpr unsigned NEON_SHIFT = 1;
		constexpr unsigned NEON_SIGN = 2;

		const auto emit_zero_result = [&]() {
			unsigned result_qreg = NEON_RT;
			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!IsGprQCacheQregResident(qreg))
	{
					result_qreg = qreg;
					break;
	}
			}

			InvalidateGprQCacheForQreg(result_qreg);
			return m_code.EmitVeorQ(result_qreg, result_qreg, result_qreg) &&
				   EmitStoreGprQ128(rd, result_qreg, HOST_TMP0);
		};

		const auto emit_sign_extend_active_words = [&](unsigned rt_qreg, unsigned sign_qreg) {
			return m_code.EmitVshrS32Q(sign_qreg, rt_qreg, 31) &&
				   m_code.EmitVtrnI32Q(rt_qreg, sign_qreg);
		};

		if (rt == 0)
			return emit_zero_result();

		const int cached_rt_qreg = FindGprQCache(rt);
		const int cached_shift_source_qreg = rs == 0 ? -1 : FindGprQCache(rs);
		bool used_qregs[MAX_GPR_QCACHE]{};
		const auto mark_used = [&](unsigned qreg) {
			if (qreg < MAX_GPR_QCACHE)
				used_qregs[qreg] = true;
		};
		const auto choose_qreg = [&]() {
			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
	{
					used_qregs[qreg] = true;
	return qreg;
	}
			}

			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg])
	{
					used_qregs[qreg] = true;
	return qreg;
	}
			}

			return MAX_GPR_QCACHE;
		};
		mark_used(cached_shift_source_qreg >= 0 ?
			static_cast<unsigned>(cached_shift_source_qreg) : MAX_GPR_QCACHE);
		unsigned rt_qreg = cached_rt_qreg >= 0 ?
			static_cast<unsigned>(cached_rt_qreg) : MAX_GPR_QCACHE;
		mark_used(rt_qreg);
		if (cached_rt_qreg < 0)
		{
			rt_qreg = choose_qreg();
			if (rt_qreg >= MAX_GPR_QCACHE ||
				!(ShouldLoadGprQ128SingleUseEntry(rt) ?
					  EmitLoadGprQ128SingleUseEntry(rt, rt_qreg, HOST_TMP0) :
					  EmitLoadGprQ128(rt, rt_qreg, HOST_TMP0)))
			{
				return false;
			}
		}

		unsigned shift_qreg = choose_qreg();
		unsigned sign_qreg = choose_qreg();
		if (shift_qreg >= MAX_GPR_QCACHE || sign_qreg >= MAX_GPR_QCACHE)
			return false;

		InvalidateGprQCacheForQreg(sign_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuMmiVariableWordShiftVectorOps++;
#endif

		if (rs != 0)
		{
			// PCSX2's x86 recPSLLVW/recPSRLVW/recPSRAVW keeps the shift counts
			// vector-resident before masking them to five bits. Copy/load the whole
			// qword: lanes 1/3 are discarded by the final VTRN, and one Q copy is
			// cheaper than moving the two architecturally active lanes separately.
			const int cached_shift_qreg = cached_shift_source_qreg;
			if (cached_shift_qreg != static_cast<int>(shift_qreg))
				InvalidateGprQCacheForQreg(shift_qreg);

			if (cached_shift_qreg >= 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheWordHits += 2;
#endif
				if (!m_code.EmitVorrQ(shift_qreg,
						static_cast<unsigned>(cached_shift_qreg),
						static_cast<unsigned>(cached_shift_qreg)))
				{
					return false;
				}
			}
			else if (!(ShouldLoadGprQ128SingleUseEntry(rs) ?
						   EmitLoadGprQ128SingleUseEntry(rs, shift_qreg, HOST_TMP0) :
						   EmitLoadGprQ128(rs, shift_qreg, HOST_TMP0)))
			{
				return false;
			}
			InvalidateGprQCacheForQreg(shift_qreg);

			if (!m_code.EmitVshlI32Q(shift_qreg, shift_qreg, 27) ||
				!m_code.EmitVshrU32Q(shift_qreg, shift_qreg, 27))
			{
				return false;
			}

			if (operation != MmiVariableWordShiftOp::ShiftLeftLogical &&
				!m_code.EmitVnegS32Q(shift_qreg, shift_qreg))
			{
				return false;
			}

			if (cached_rt_qreg >= 0 || rt_qreg != NEON_RT)
			{
				if (rd != rt &&
					!PreserveGprQCacheGuestForFutureRead(rt, rt_qreg,
						shift_qreg, sign_qreg, nullptr))
				{
					return false;
				}
				InvalidateGprQCacheForQreg(rt_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectVariableShiftOps++;
#endif
			}

			bool shift_ok = true;
			switch (operation)
			{
				case MmiVariableWordShiftOp::ShiftLeftLogical:
					shift_ok = m_code.EmitVshlU32Q(rt_qreg, rt_qreg, shift_qreg);
					break;
				case MmiVariableWordShiftOp::ShiftRightLogical:
					shift_ok = m_code.EmitVshlU32Q(rt_qreg, rt_qreg, shift_qreg);
					break;
				case MmiVariableWordShiftOp::ShiftRightArithmetic:
					shift_ok = m_code.EmitVshlS32Q(rt_qreg, rt_qreg, shift_qreg);
					break;
			}

			if (!shift_ok)
				return false;
		}
		else if (cached_rt_qreg >= 0 || rt_qreg != NEON_RT)
		{
			if (rd != rt &&
				!PreserveGprQCacheGuestForFutureRead(rt, rt_qreg,
					sign_qreg, MAX_GPR_QCACHE, nullptr))
			{
				return false;
			}
			InvalidateGprQCacheForQreg(rt_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheDirectVariableShiftOps++;
#endif
		}

		return emit_sign_extend_active_words(rt_qreg, sign_qreg) &&
			   EmitStoreGprQ128(rd, rt_qreg, HOST_TMP2);
	}

	bool BlockCompiler::EmitMmiWordShuffleOp(u32 op, MmiWordShuffleOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_DWORD_PAIR = 0;
		constexpr unsigned NEON_LOW_D = NEON_DWORD_PAIR * 2;
		constexpr unsigned NEON_HIGH_D = NEON_DWORD_PAIR * 2 + 1;

		const auto emit_zero_qword = [&]() {
			return m_code.EmitVeorQ(NEON_DWORD_PAIR, NEON_DWORD_PAIR, NEON_DWORD_PAIR) &&
				   EmitStoreGprQ128(rd, NEON_DWORD_PAIR, HOST_TMP2);
		};

		const auto should_load_single_use_entry_operand = [&](unsigned guest_reg) {
			return ShouldLoadGprQ128SingleUseEntry(guest_reg);
		};
		const auto load_qword_operand = [&](unsigned guest_reg, unsigned qreg,
										 unsigned address_scratch) {
			if (should_load_single_use_entry_operand(guest_reg))
				return EmitLoadGprQ128SingleUseEntry(guest_reg, qreg,
					address_scratch);
			return EmitLoadGprQ128(guest_reg, qreg, address_scratch);
		};

		const auto emit_copy_dword_pair = [&](unsigned low_guest_reg, unsigned low_dword,
			unsigned high_guest_reg, unsigned high_dword) {
			if (low_guest_reg == high_guest_reg && low_dword == high_dword)
			{
				// PCSX2 x86/iMMI.cpp::recPCPYLD()/recPCPYUD() special-case
				// EEREC_S == EEREC_T with a shuffle. Mirror that on A32 by
				// materializing the selected D half once and duplicating it.
				if (low_guest_reg == 0)
					return emit_zero_qword();

				const int cached_qreg = FindGprQCache(low_guest_reg);
				const auto choose_result_qreg = [&]() {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
						if (cached_qreg != static_cast<int>(qreg) &&
							!IsGprQCacheQregResident(qreg))
						{
							return qreg;
						}
	}

					return NEON_DWORD_PAIR;
	};
				const unsigned result_qreg = choose_result_qreg();
				const unsigned result_low_d = result_qreg * 2;
				const unsigned result_high_d = result_low_d + 1;
				InvalidateGprQCacheForQreg(result_qreg);
				if (cached_qreg >= 0)
	{
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuGprQCacheDwordHits++;
#endif
					const unsigned source_d = static_cast<unsigned>(cached_qreg) * 2 + low_dword;
					return (source_d == result_low_d ||
							   m_code.EmitVorrD(result_low_d, source_d, source_d)) &&
						   m_code.EmitVorrD(result_high_d, result_low_d, result_low_d) &&
						   EmitStoreGprQ128(rd, result_qreg, HOST_TMP2);
	}

				return m_code.EmitVldrDImm(result_low_d, HOST_CPU_REGS,
						   static_cast<u16>(GprOffset(low_guest_reg) + low_dword * sizeof(u64))) &&
					   m_code.EmitVorrD(result_high_d, result_low_d, result_low_d) &&
					   EmitStoreGprQ128(rd, result_qreg, HOST_TMP2);
			}

			struct DwordSource
			{
				unsigned guest_reg;
				unsigned dword;
				unsigned target_d;
				int cached_qreg = -1;
				bool emitted_cache = false;
			};

			DwordSource low{low_guest_reg, low_dword, NEON_LOW_D};
			DwordSource high{high_guest_reg, high_dword, NEON_HIGH_D};
			if (low.guest_reg != 0)
				low.cached_qreg = FindGprQCache(low.guest_reg);
			if (high.guest_reg != 0)
				high.cached_qreg = FindGprQCache(high.guest_reg);

			const auto emit_direct_dword_pair = [&](unsigned result_qreg) {
				const unsigned low_target_d = result_qreg * 2;
				const unsigned high_target_d = low_target_d + 1;
				const auto cached_source_d_for = [](const DwordSource& source) {
					return static_cast<unsigned>(source.cached_qreg) * 2 + source.dword;
	};
				const auto emit_source = [&](const DwordSource& source, unsigned target_d) {
					if (source.cached_qreg >= 0)
	{
						const unsigned source_d = cached_source_d_for(source);
						return source_d == target_d ||
							   m_code.EmitVorrD(target_d, source_d, source_d);
	}

					if (source.guest_reg == 0)
						return m_code.EmitVeorD(target_d, target_d, target_d);

					return m_code.EmitVldrDImm(target_d, HOST_CPU_REGS,
						static_cast<u16>(GprOffset(source.guest_reg) + source.dword * sizeof(u64)));
	};

				InvalidateGprQCacheForQreg(result_qreg);
				const bool low_source_would_be_clobbered =
					low.cached_qreg == static_cast<int>(result_qreg) &&
					cached_source_d_for(low) == high_target_d &&
					cached_source_d_for(low) != low_target_d;
				const bool high_source_would_be_clobbered =
					high.cached_qreg == static_cast<int>(result_qreg) &&
					cached_source_d_for(high) == low_target_d &&
					cached_source_d_for(high) != high_target_d;
				bool low_emitted = false;
				bool high_emitted = false;
				const auto emit_low = [&]() {
					if (low_emitted)
						return true;
					low_emitted = true;
					return emit_source(low, low_target_d);
	};
				const auto emit_high = [&]() {
					if (high_emitted)
						return true;
					high_emitted = true;
					return emit_source(high, high_target_d);
	};

				if ((low_source_would_be_clobbered && !emit_low()) ||
					(high_source_would_be_clobbered && !emit_high()) ||
					!emit_low() ||
					!emit_high() ||
					!EmitStoreGprQ128(rd, result_qreg, HOST_TMP2))
	{
					return false;
	}

#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectDwordPairOps++;
#endif
				return true;
			};

			if (low.cached_qreg >= 0)
				return emit_direct_dword_pair(static_cast<unsigned>(low.cached_qreg));
			if (high.cached_qreg >= 0)
				return emit_direct_dword_pair(static_cast<unsigned>(high.cached_qreg));

			const auto cached_source_d = [](const DwordSource& source) {
				return static_cast<unsigned>(source.cached_qreg) * 2 + source.dword;
			};

			const auto emit_cached_source = [&](DwordSource& source) {
				if (source.cached_qreg < 0 || source.emitted_cache)
					return true;

				source.emitted_cache = true;
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDwordHits++;
#endif
				const unsigned source_d = cached_source_d(source);
				return source_d == source.target_d ||
					   m_code.EmitVorrD(source.target_d, source_d, source_d);
			};

			const auto q0_source_would_be_clobbered = [&](const DwordSource& source) {
				return source.cached_qreg == static_cast<int>(NEON_DWORD_PAIR) &&
					   cached_source_d(source) != source.target_d;
			};

			// Copy any q0-backed source half before the other half writes D0/D1.
			if ((q0_source_would_be_clobbered(low) && !emit_cached_source(low)) ||
				(q0_source_would_be_clobbered(high) && !emit_cached_source(high)) ||
				!emit_cached_source(low) ||
				!emit_cached_source(high))
			{
				return false;
			}

			const auto emit_uncached_source = [&](const DwordSource& source) {
				if (source.cached_qreg >= 0)
					return true;
				if (source.guest_reg == 0)
					return m_code.EmitVeorD(source.target_d, source.target_d, source.target_d);

				return m_code.EmitVldrDImm(source.target_d, HOST_CPU_REGS,
					static_cast<u16>(GprOffset(source.guest_reg) + source.dword * sizeof(u64)));
			};

			if (!emit_uncached_source(low) || !emit_uncached_source(high))
				return false;

			return EmitStoreGprQ128(rd, NEON_DWORD_PAIR, HOST_TMP2);
		};

		const auto emit_store_rd_from_q = [&](unsigned qreg) {
			if (rd == rt && rt != 0)
			{
				return EmitCpuRegsAddress(HOST_TMP0, GprOffset(rd)) &&
					   EmitStoreGprQ128ToAddress(rd, qreg, HOST_TMP0);
			}

			return EmitStoreGprQ128(rd, qreg, HOST_TMP2);
		};

		const auto emit_shuffle_qreg_words = [&](MmiWordShuffleOp shuffle, unsigned qreg) {
			const unsigned low_d = qreg * 2;
			const unsigned high_d = low_d + 1;
			switch (shuffle)
			{
				case MmiWordShuffleOp::Pexew:
					return m_code.EmitVtrnI32D(low_d, high_d) &&
						   m_code.EmitVrev64I32D(low_d, low_d) &&
						   m_code.EmitVtrnI32D(low_d, high_d);
				case MmiWordShuffleOp::Prot3w:
					return m_code.EmitVextI8Q(qreg, qreg, qreg, sizeof(u32)) &&
						   m_code.EmitVrev64I32D(high_d, high_d);
				case MmiWordShuffleOp::Pexcw:
					return m_code.EmitVtrnI32D(low_d, high_d);
				default:
					return false;
			}
		};

		const auto emit_shuffle_rt_words = [&](MmiWordShuffleOp shuffle) {
			if (rt == 0)
				return emit_zero_qword();

			const auto choose_rt_qreg = [&]() {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (!IsGprQCacheQregResident(qreg))
						return qreg;
	}

				return NEON_DWORD_PAIR;
			};

			const int cached_qreg = FindGprQCache(rt);
			if (cached_qreg >= 0)
			{
				const unsigned qreg = static_cast<unsigned>(cached_qreg);
				if (rd != rt &&
					!PreserveGprQCacheGuestForFutureRead(rt, qreg, qreg,
						MAX_GPR_QCACHE, nullptr))
				{
					return false;
				}
				InvalidateGprQCacheForQreg(qreg);
				if (!emit_shuffle_qreg_words(shuffle, qreg) ||
					!emit_store_rd_from_q(qreg))
	{
					return false;
	}
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectWordShuffleOps++;
				g_qemuMmiWordShuffleVectorOps++;
#endif
				return true;
			}

			const unsigned rt_qreg = choose_rt_qreg();
			if (!load_qword_operand(rt, rt_qreg, HOST_TMP0))
				return false;

#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiWordShuffleVectorOps++;
#endif
			return emit_shuffle_qreg_words(shuffle, rt_qreg) &&
				   emit_store_rd_from_q(rt_qreg);
		};

		switch (operation)
		{
			case MmiWordShuffleOp::Pcpyld:
				// PCSX2 owners: MMI.cpp::PCPYLD() and x86/iMMI.cpp::recPCPYLD().
				// Load both halves before storing so rd==rs/rt stays alias-safe.
				return emit_copy_dword_pair(rt, 0, rs, 0);
			case MmiWordShuffleOp::Pcpyud:
				// PCSX2 owners: MMI.cpp::PCPYUD() and x86/iMMI.cpp::recPCPYUD().
				return emit_copy_dword_pair(rs, 1, rt, 1);
			case MmiWordShuffleOp::Pexew:
				// PCSX2 owners: MMI.cpp::PEXEW() and x86/iMMI.cpp::recPEXEW().
				return emit_shuffle_rt_words(operation);
			case MmiWordShuffleOp::Prot3w:
				// PCSX2 owners: MMI.cpp::PROT3W() and x86/iMMI.cpp::recPROT3W().
				return emit_shuffle_rt_words(operation);
			case MmiWordShuffleOp::Pexcw:
				// PCSX2 owners: MMI.cpp::PEXCW() and x86/iMMI.cpp::recPEXCW().
				return emit_shuffle_rt_words(operation);
		}

		return false;
	}

	bool BlockCompiler::EmitMmiFiveBitOp(u32 op, MmiFiveBitOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RT = 0;
		constexpr unsigned NEON_FIELD0 = 1;
		constexpr unsigned NEON_FIELD1 = 2;
		constexpr unsigned NEON_FIELD2 = 3;

		const auto emit_store_rd_q = [&](unsigned qreg) {
			if (rd == rt && rt != 0)
			{
				return EmitCpuRegsAddress(HOST_TMP0, GprOffset(rd)) &&
					   EmitStoreGprQ128ToAddress(rd, qreg, HOST_TMP0);
			}

			return EmitStoreGprQ128(rd, qreg, HOST_TMP2);
		};

		if (rt == 0)
		{
			return m_code.EmitVeorQ(NEON_RT, NEON_RT, NEON_RT) &&
				   emit_store_rd_q(NEON_RT);
		}

		bool used_qregs[MAX_GPR_QCACHE]{};
		const auto should_load_single_use_entry_operand = [&](unsigned guest_reg) {
			return ShouldLoadGprQ128SingleUseEntry(guest_reg);
		};
		const auto load_qword_operand = [&](unsigned guest_reg, unsigned qreg,
										 unsigned address_scratch) {
			if (should_load_single_use_entry_operand(guest_reg))
				return EmitLoadGprQ128SingleUseEntry(guest_reg, qreg,
					address_scratch);
			return EmitLoadGprQ128(guest_reg, qreg, address_scratch);
		};
		const auto choose_qreg = [&]() {
			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
	{
					used_qregs[qreg] = true;
	return qreg;
	}
			}

			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg])
	{
					used_qregs[qreg] = true;
	return qreg;
	}
			}

			return MAX_GPR_QCACHE;
		};

		unsigned rt_qreg = NEON_RT;
		unsigned field0_qreg = NEON_FIELD0;
		unsigned field1_qreg = NEON_FIELD1;
		unsigned field2_qreg = NEON_FIELD2;
		const int cached_qreg = FindGprQCache(rt);
		if (cached_qreg >= 0)
		{
			rt_qreg = static_cast<unsigned>(cached_qreg);
			used_qregs[rt_qreg] = true;
			if (rd != rt &&
				!PreserveGprQCacheGuestForFutureRead(rt, rt_qreg, rt_qreg,
					MAX_GPR_QCACHE, nullptr))
			{
				return false;
			}
			field0_qreg = choose_qreg();
			field1_qreg = choose_qreg();
			field2_qreg = choose_qreg();
			if (field0_qreg >= MAX_GPR_QCACHE || field1_qreg >= MAX_GPR_QCACHE ||
				field2_qreg >= MAX_GPR_QCACHE)
			{
				return false;
			}
			InvalidateGprQCacheForQreg(rt_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheDirectFiveBitOps++;
#endif
		}
		else
		{
			rt_qreg = choose_qreg();
			field0_qreg = choose_qreg();
			field1_qreg = choose_qreg();
			field2_qreg = choose_qreg();
			if (rt_qreg >= MAX_GPR_QCACHE || field0_qreg >= MAX_GPR_QCACHE ||
				field1_qreg >= MAX_GPR_QCACHE || field2_qreg >= MAX_GPR_QCACHE ||
				!load_qword_operand(rt, rt_qreg, HOST_TMP0))
			{
				return false;
			}
		}

		InvalidateGprQCacheForQreg(field0_qreg);
		InvalidateGprQCacheForQreg(field1_qreg);
		InvalidateGprQCacheForQreg(field2_qreg);

#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuMmiFiveBitVectorOps++;
#endif

		// PCSX2 owners: MMI.cpp::PEXT5()/PPAC5() and x86/iMMI.cpp::recPEXT5()/
		// recPPAC5(). Shifts isolate each 5:5:5:1 field per 32-bit lane without
		// materializing vector masks.
		if (operation == MmiFiveBitOp::Expand)
		{
			return m_code.EmitVshlI32Q(field0_qreg, rt_qreg, 27) &&
				   m_code.EmitVshrU32Q(field0_qreg, field0_qreg, 27) &&
				   m_code.EmitVshlI32Q(field0_qreg, field0_qreg, 3) &&
				   m_code.EmitVshlI32Q(field1_qreg, rt_qreg, 22) &&
				   m_code.EmitVshrU32Q(field1_qreg, field1_qreg, 27) &&
				   m_code.EmitVshlI32Q(field1_qreg, field1_qreg, 11) &&
				   m_code.EmitVshlI32Q(field2_qreg, rt_qreg, 17) &&
				   m_code.EmitVshrU32Q(field2_qreg, field2_qreg, 27) &&
				   m_code.EmitVshlI32Q(field2_qreg, field2_qreg, 19) &&
				   m_code.EmitVshrU32Q(rt_qreg, rt_qreg, 15) &&
				   m_code.EmitVshlI32Q(rt_qreg, rt_qreg, 31) &&
				   m_code.EmitVorrQ(field0_qreg, field0_qreg, field1_qreg) &&
				   m_code.EmitVorrQ(field0_qreg, field0_qreg, field2_qreg) &&
				   m_code.EmitVorrQ(rt_qreg, rt_qreg, field0_qreg) &&
				   emit_store_rd_q(rt_qreg);
		}

		return m_code.EmitVshlI32Q(field0_qreg, rt_qreg, 24) &&
			   m_code.EmitVshrU32Q(field0_qreg, field0_qreg, 27) &&
			   m_code.EmitVshlI32Q(field1_qreg, rt_qreg, 16) &&
			   m_code.EmitVshrU32Q(field1_qreg, field1_qreg, 27) &&
			   m_code.EmitVshlI32Q(field1_qreg, field1_qreg, 5) &&
			   m_code.EmitVshlI32Q(field2_qreg, rt_qreg, 8) &&
			   m_code.EmitVshrU32Q(field2_qreg, field2_qreg, 27) &&
			   m_code.EmitVshlI32Q(field2_qreg, field2_qreg, 10) &&
			   m_code.EmitVshrU32Q(rt_qreg, rt_qreg, 31) &&
			   m_code.EmitVshlI32Q(rt_qreg, rt_qreg, 15) &&
			   m_code.EmitVorrQ(field0_qreg, field0_qreg, field1_qreg) &&
			   m_code.EmitVorrQ(field0_qreg, field0_qreg, field2_qreg) &&
			   m_code.EmitVorrQ(rt_qreg, rt_qreg, field0_qreg) &&
			   emit_store_rd_q(rt_qreg);
	}

	bool BlockCompiler::EmitMmiHalfwordShuffleOp(u32 op, MmiHalfwordShuffleOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RT = 0;
		constexpr unsigned NEON_RS = 1;

		const auto choose_nonresident_qreg = [&]() {
			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!IsGprQCacheQregResident(qreg))
	return qreg;
			}

			return NEON_RT;
		};

		const auto emit_zero_rt_result = [&]() {
			const unsigned result_qreg = choose_nonresident_qreg();
			InvalidateGprQCacheForQreg(result_qreg);
			return m_code.EmitVeorQ(result_qreg, result_qreg, result_qreg) &&
				   EmitStoreGprQ128(rd, result_qreg, HOST_TMP2);
		};

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			return EmitCpuRegsAddress(host_reg, GprOffset(guest_reg));
		};

		const auto emit_load_q = [&](unsigned guest_reg, unsigned qreg, unsigned address_reg) {
			if (ShouldLoadGprQ128SingleUseEntry(guest_reg))
				return EmitLoadGprQ128SingleUseEntry(guest_reg, qreg, address_reg);

			return EmitLoadGprQ128(guest_reg, qreg, address_reg);
		};

		const auto emit_store_rd_q = [&]() {
			if (rd == rt && rt != 0)
			{
				return emit_gpr_address(rd, HOST_TMP0) &&
					   EmitStoreGprQ128ToAddress(rd, NEON_RT, HOST_TMP0);
			}

			if (rd == rs && rs != 0)
			{
				return emit_gpr_address(rd, HOST_TMP1) &&
					   EmitStoreGprQ128ToAddress(rd, NEON_RT, HOST_TMP1);
			}

			return EmitStoreGprQ128(rd, NEON_RT, HOST_TMP2);
		};

		const auto emit_store_rd_rt_q = [&]() {
			if (rd == rt && rt != 0)
			{
				return emit_gpr_address(rd, HOST_TMP0) &&
					   EmitStoreGprQ128ToAddress(rd, NEON_RT, HOST_TMP0);
			}

			return EmitStoreGprQ128(rd, NEON_RT, HOST_TMP2);
		};

		const auto emit_store_rd_q_after_rs_clobber = [&]() {
			InvalidateGprQCacheForQreg(NEON_RS);
			return emit_store_rd_q();
		};

		const auto emit_store_rd_rt_q_after_rs_clobber = [&]() {
			InvalidateGprQCacheForQreg(NEON_RS);
			return emit_store_rd_rt_q();
		};

		const auto emit_store_rd_from_q = [&](unsigned qreg) {
			if (rd == rt && rt != 0)
			{
				return emit_gpr_address(rd, HOST_TMP0) &&
					   EmitStoreGprQ128ToAddress(rd, qreg, HOST_TMP0);
			}

			if (rd == rs && rs != 0)
			{
				return emit_gpr_address(rd, HOST_TMP1) &&
					   EmitStoreGprQ128ToAddress(rd, qreg, HOST_TMP1);
			}

			return EmitStoreGprQ128(rd, qreg, HOST_TMP2);
		};

		const auto choose_tmp_qreg = [&](unsigned source_qreg) {
			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (qreg != source_qreg && !IsGprQCacheQregResident(qreg))
	return qreg;
			}

			return source_qreg == NEON_RS ? NEON_RT : NEON_RS;
		};

		bool cached_unary_shuffle_attempted = false;
		const auto try_emit_cached_unary_shuffle = [&]() -> bool {
			if (rt == 0)
				return false;

			switch (operation)
			{
				case MmiHalfwordShuffleOp::Pexeh:
				case MmiHalfwordShuffleOp::Prevh:
				case MmiHalfwordShuffleOp::Pexch:
				case MmiHalfwordShuffleOp::Pcpyh:
					break;
				default:
					return false;
			}

			const int cached_qreg = FindGprQCache(rt);
			if (cached_qreg < 0)
				return false;

			cached_unary_shuffle_attempted = true;
			const unsigned source_qreg = static_cast<unsigned>(cached_qreg);
			unsigned tmp_qreg = MAX_GPR_QCACHE;
			if (rd != rt &&
				!PreserveGprQCacheGuestForFutureRead(rt, source_qreg,
					source_qreg, MAX_GPR_QCACHE, nullptr))
			{
				return false;
			}
			InvalidateGprQCacheForQreg(source_qreg);

			bool shuffle_ok = false;
			switch (operation)
			{
				case MmiHalfwordShuffleOp::Pexeh:
					tmp_qreg = choose_tmp_qreg(source_qreg);
					InvalidateGprQCacheForQreg(tmp_qreg);
					shuffle_ok = m_code.EmitVorrQ(tmp_qreg, source_qreg, source_qreg) &&
								 m_code.EmitVtrnI16Q(source_qreg, tmp_qreg) &&
								 m_code.EmitVrev64I32Q(source_qreg, source_qreg) &&
								 m_code.EmitVtrnI16Q(source_qreg, tmp_qreg);
					break;
				case MmiHalfwordShuffleOp::Prevh:
					shuffle_ok = m_code.EmitVrev64I16Q(source_qreg, source_qreg);
					break;
				case MmiHalfwordShuffleOp::Pexch:
					tmp_qreg = choose_tmp_qreg(source_qreg);
					InvalidateGprQCacheForQreg(tmp_qreg);
					shuffle_ok = m_code.EmitVorrQ(tmp_qreg, source_qreg, source_qreg) &&
								 m_code.EmitVuzpI16Q(source_qreg, tmp_qreg) &&
								 m_code.EmitVzipI32Q(source_qreg, tmp_qreg);
					break;
				case MmiHalfwordShuffleOp::Pcpyh:
	{
					const unsigned low_d = source_qreg * 2;
					const unsigned high_d = low_d + 1;
					shuffle_ok = m_code.EmitVdupI16D(low_d, low_d, 0) &&
								 m_code.EmitVdupI16D(high_d, high_d, 0);
					break;
	}
				default:
					break;
			}

			if (!shuffle_ok ||
				!emit_store_rd_from_q(source_qreg))
			{
				return false;
			}

#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheDirectHalfwordShuffleOps++;
			g_qemuMmiHalfwordShuffleVectorOps++;
#endif
			return true;
		};

		if (try_emit_cached_unary_shuffle())
			return true;
		if (cached_unary_shuffle_attempted)
			return false;

		const auto emit_two_source_shuffle = [&](unsigned rt_qreg, unsigned rs_qreg) {
			switch (operation)
			{
				case MmiHalfwordShuffleOp::Pinth:
					return m_code.EmitVextI8Q(rs_qreg, rs_qreg, rs_qreg, 8) &&
						   m_code.EmitVzipI16Q(rt_qreg, rs_qreg);
				case MmiHalfwordShuffleOp::Pinteh:
					return m_code.EmitVuzpI16Q(rt_qreg, rs_qreg) &&
						   m_code.EmitVextI8Q(rs_qreg, rt_qreg, rt_qreg, 8) &&
						   m_code.EmitVzipI16Q(rt_qreg, rs_qreg);
				default:
					return false;
			}
		};

		const auto emit_cached_two_source_shuffle = [&](unsigned rt_qreg, unsigned rs_qreg,
			bool mixed) {
			if (rt_qreg == rs_qreg)
				return false;

			bool preserved_rt = false;
			bool preserved_rs = false;
			if (rd != rt &&
				!PreserveGprQCacheGuestForFutureRead(rt, rt_qreg, rt_qreg, rs_qreg,
					&preserved_rt))
			{
				return false;
			}
			if (rd != rs &&
				!PreserveGprQCacheGuestForFutureRead(rs, rs_qreg, rs_qreg, rt_qreg,
					&preserved_rs))
			{
				return false;
			}
			InvalidateGprQCacheForQreg(rs_qreg);
			InvalidateGprQCacheForQreg(rt_qreg);
			if (!emit_two_source_shuffle(rt_qreg, rs_qreg) ||
				!emit_store_rd_from_q(rt_qreg))
			{
				return false;
			}

#if defined(VITASX2_QEMU_VALIDATION)
			if (mixed)
				g_qemuGprQCacheDirectMixedHalfwordShuffleOps++;
			else
				g_qemuGprQCacheDirectHalfwordShuffleOps++;
			g_qemuMmiHalfwordShuffleVectorOps++;
#endif
			return true;
		};

		if (operation == MmiHalfwordShuffleOp::Pinth ||
			operation == MmiHalfwordShuffleOp::Pinteh)
		{
			const int cached_rt_qreg = rt == 0 ? -1 : FindGprQCache(rt);
			const int cached_rs_qreg = rs == 0 ? -1 : FindGprQCache(rs);
			if (cached_rt_qreg >= 0 && cached_rs_qreg >= 0 &&
				cached_rt_qreg != cached_rs_qreg)
			{
				return emit_cached_two_source_shuffle(static_cast<unsigned>(cached_rt_qreg),
	static_cast<unsigned>(cached_rs_qreg), false);
			}

			if (cached_rt_qreg >= 0 && rs != 0)
			{
	const unsigned rt_qreg = static_cast<unsigned>(cached_rt_qreg);
				const auto choose_source_qreg = [&](unsigned avoid) {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
						if (qreg != avoid && !IsGprQCacheQregResident(qreg))
							return qreg;
	}

					return avoid == NEON_RS ? NEON_RT : NEON_RS;
	};
				const unsigned rs_qreg = choose_source_qreg(rt_qreg);
				if (rs_qreg == rt_qreg)
					return false;
				if (!EmitLoadGprQ128(rs, rs_qreg, HOST_TMP1))
					return false;
				return emit_cached_two_source_shuffle(rt_qreg, rs_qreg, true);
			}

			if (cached_rs_qreg >= 0 && rt != 0)
			{
				const unsigned rs_qreg = static_cast<unsigned>(cached_rs_qreg);
				const auto choose_source_qreg = [&](unsigned avoid) {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
						if (qreg != avoid && !IsGprQCacheQregResident(qreg))
							return qreg;
	}

					return avoid == NEON_RT ? NEON_RS : NEON_RT;
	};
	const unsigned rt_qreg = choose_source_qreg(rs_qreg);
				if (rt_qreg == rs_qreg)
					return false;
				if (!EmitLoadGprQ128(rt, rt_qreg, HOST_TMP0))
					return false;
				return emit_cached_two_source_shuffle(rt_qreg, rs_qreg, true);
			}
		}

		if (operation == MmiHalfwordShuffleOp::Pinth)
		{
			// PCSX2 owners: MMI.cpp::PINTH() and x86/iMMI.cpp::recPINTH().
			// RT low halfwords are interleaved with RS upper halfwords.
			if (rt == 0 && rs == 0)
				return emit_zero_rt_result();

			if (rt == 0 || rs == 0)
			{
				const unsigned source_guest = rt == 0 ? rs : rt;
				const int cached_source_qreg = FindGprQCache(source_guest);
				bool used_qregs[MAX_GPR_QCACHE]{};
				if (cached_source_qreg >= 0)
					used_qregs[static_cast<unsigned>(cached_source_qreg)] = true;
				const auto choose_scratch_qreg = [&]() {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
						if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
						{
							used_qregs[qreg] = true;
							return qreg;
						}
	}

	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
						if (!used_qregs[qreg])
						{
							used_qregs[qreg] = true;
							return qreg;
						}
	}

					return MAX_GPR_QCACHE;
	};

	const unsigned rt_qreg = choose_scratch_qreg();
				const unsigned rs_qreg = choose_scratch_qreg();
				if (rt_qreg >= MAX_GPR_QCACHE || rs_qreg >= MAX_GPR_QCACHE)
					return false;

				const unsigned rt_d = rt_qreg * 2;
				const unsigned rs_d = rs_qreg * 2;
				InvalidateGprQCacheForQreg(rt_qreg);
				InvalidateGprQCacheForQreg(rs_qreg);
				const auto emit_source_d = [&](unsigned guest_reg, unsigned source_half,
					unsigned target_d) {
					if (guest_reg == 0)
						return m_code.EmitVeorD(target_d, target_d, target_d);

					const int cached_qreg = FindGprQCache(guest_reg);
					if (cached_qreg >= 0)
	{
#if defined(VITASX2_QEMU_VALIDATION)
						g_qemuGprQCacheDwordHits++;
#endif
						const unsigned source_d =
							static_cast<unsigned>(cached_qreg) * 2 + source_half;
						return source_d == target_d ||
							   m_code.EmitVorrD(target_d, source_d, source_d);
	}

					if (!EmitFlushDirtyGprPinsForGuest(guest_reg))
						return false;

					return m_code.EmitVldrDImm(target_d, HOST_CPU_REGS,
						static_cast<u16>(GprOffset(guest_reg) + source_half * sizeof(u64)));
	};

				if (!emit_source_d(rt, 0, rt_d) ||
					!emit_source_d(rs, 1, rs_d) ||
					!m_code.EmitVzipI16Q(rt_qreg, rs_qreg) ||
					!emit_store_rd_from_q(rt_qreg))
	{
					return false;
	}

#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectHalfwordShuffleOps++;
				g_qemuMmiHalfwordShuffleVectorOps++;
#endif
				return true;
			}

#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiHalfwordShuffleVectorOps++;
#endif
			return emit_load_q(rt, NEON_RT, HOST_TMP0) &&
				   emit_load_q(rs, NEON_RS, HOST_TMP1) &&
				   m_code.EmitVextI8Q(NEON_RS, NEON_RS, NEON_RS, 8) &&
				   m_code.EmitVzipI16Q(NEON_RT, NEON_RS) &&
				   emit_store_rd_q_after_rs_clobber();
		}

		if (operation == MmiHalfwordShuffleOp::Pinteh)
		{
			// PCSX2 owners: MMI.cpp::PINTEH() and x86/iMMI.cpp::recPINTEH().
			// Keep even halfwords from each source, then interleave RT/RS.
			if (rt == 0 && rs == 0)
				return emit_zero_rt_result();

			if (rt == 0 || rs == 0)
			{
				const unsigned source_guest = rt == 0 ? rs : rt;
				const int cached_source_qreg = FindGprQCache(source_guest);
				const auto choose_source_qreg = [&](unsigned avoid) {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
						if (qreg != avoid && !IsGprQCacheQregResident(qreg))
							return qreg;
	}

					return avoid == NEON_RT ? NEON_RS : NEON_RT;
	};

				unsigned rt_qreg = NEON_RT;
				unsigned rs_qreg = NEON_RS;
				const bool rt_is_zero = rt == 0;
				if (cached_source_qreg >= 0)
	{
					const unsigned source_qreg = static_cast<unsigned>(cached_source_qreg);
					const unsigned zero_qreg = choose_source_qreg(source_qreg);
					if (zero_qreg == source_qreg)
						return false;

					rt_qreg = rt_is_zero ? zero_qreg : source_qreg;
					rs_qreg = rt_is_zero ? source_qreg : zero_qreg;
					InvalidateGprQCacheForQreg(zero_qreg);
					if (!m_code.EmitVeorQ(zero_qreg, zero_qreg, zero_qreg))
						return false;
	}
				else
	{
					if (rt_is_zero)
	{
						rt_qreg = choose_source_qreg(NEON_RS);
						rs_qreg = NEON_RS;
	}
					else
	{
						rt_qreg = NEON_RT;
						rs_qreg = choose_source_qreg(NEON_RT);
	}
					if (rt_qreg == rs_qreg)
						return false;

					InvalidateGprQCacheForQreg(rt_qreg);
					InvalidateGprQCacheForQreg(rs_qreg);
					if (!EmitLoadGprQ128(source_guest, rt_is_zero ? rs_qreg : rt_qreg,
							rt_is_zero ? HOST_TMP1 : HOST_TMP0) ||
						!m_code.EmitVeorQ(rt_is_zero ? rt_qreg : rs_qreg,
							rt_is_zero ? rt_qreg : rs_qreg,
							rt_is_zero ? rt_qreg : rs_qreg))
	{
						return false;
	}
	}

				if (!emit_two_source_shuffle(rt_qreg, rs_qreg) ||
					!emit_store_rd_from_q(rt_qreg))
	{
					return false;
	}

#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectMixedHalfwordShuffleOps++;
				g_qemuMmiHalfwordShuffleVectorOps++;
				g_qemuMmiHalfwordShuffleZeroSourceOps++;
#endif
				return true;
			}

#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiHalfwordShuffleVectorOps++;
#endif
			return emit_load_q(rt, NEON_RT, HOST_TMP0) &&
				   emit_load_q(rs, NEON_RS, HOST_TMP1) &&
				   m_code.EmitVuzpI16Q(NEON_RT, NEON_RS) &&
				   m_code.EmitVextI8Q(NEON_RS, NEON_RT, NEON_RT, 8) &&
				   m_code.EmitVzipI16Q(NEON_RT, NEON_RS) &&
				   emit_store_rd_q_after_rs_clobber();
		}

		if (operation == MmiHalfwordShuffleOp::Pexch)
		{
			// PCSX2 owners: MMI.cpp::PEXCH() and x86/iMMI.cpp::recPEXCH().
			// Duplicate RT, split even/odd halfwords, then zip 32-bit pairs.
			if (rt == 0)
				return emit_zero_rt_result();

			const unsigned rt_qreg = choose_nonresident_qreg();
			const unsigned tmp_qreg = choose_tmp_qreg(rt_qreg);
			InvalidateGprQCacheForQreg(tmp_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiHalfwordShuffleVectorOps++;
#endif
			return emit_load_q(rt, rt_qreg, HOST_TMP0) &&
				   m_code.EmitVorrQ(tmp_qreg, rt_qreg, rt_qreg) &&
				   m_code.EmitVuzpI16Q(rt_qreg, tmp_qreg) &&
				   m_code.EmitVzipI32Q(rt_qreg, tmp_qreg) &&
				   emit_store_rd_from_q(rt_qreg);
		}

		if (operation == MmiHalfwordShuffleOp::Prevh)
		{
			// PCSX2 owners: MMI.cpp::PREVH() and x86/iMMI.cpp::recPREVH().
			// PREVH reverses halfwords independently inside each 64-bit half.
			if (rt == 0)
				return emit_zero_rt_result();

			const unsigned rt_qreg = choose_nonresident_qreg();
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiHalfwordShuffleVectorOps++;
#endif
			return emit_load_q(rt, rt_qreg, HOST_TMP0) &&
				   m_code.EmitVrev64I16Q(rt_qreg, rt_qreg) &&
				   emit_store_rd_from_q(rt_qreg);
		}

		if (operation == MmiHalfwordShuffleOp::Pexeh)
		{
			// PCSX2 owners: MMI.cpp::PEXEH() and x86/iMMI.cpp::recPEXEH().
			// Duplicate even and odd halfword lanes with VTRN.16, swap the
			// duplicated even lane pairs inside each 64-bit half, then
			// interleave back to RT.US[2], RT.US[1], RT.US[0], RT.US[3].
			if (rt == 0)
				return emit_zero_rt_result();

			const unsigned rt_qreg = choose_nonresident_qreg();
			const unsigned tmp_qreg = choose_tmp_qreg(rt_qreg);
			InvalidateGprQCacheForQreg(tmp_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiHalfwordShuffleVectorOps++;
#endif
			return emit_load_q(rt, rt_qreg, HOST_TMP0) &&
				   m_code.EmitVorrQ(tmp_qreg, rt_qreg, rt_qreg) &&
				   m_code.EmitVtrnI16Q(rt_qreg, tmp_qreg) &&
				   m_code.EmitVrev64I32Q(rt_qreg, rt_qreg) &&
				   m_code.EmitVtrnI16Q(rt_qreg, tmp_qreg) &&
				   emit_store_rd_from_q(rt_qreg);
		}

		if (operation == MmiHalfwordShuffleOp::Pcpyh)
		{
			// PCSX2 owners: MMI.cpp::PCPYH() and x86/iMMI.cpp::recPCPYH().
			// Broadcast the first halfword independently within each 64-bit half.
			if (rt == 0)
				return emit_zero_rt_result();

			const unsigned rt_qreg = choose_nonresident_qreg();
			const unsigned rt_low_d = rt_qreg * 2;
			const unsigned rt_high_d = rt_low_d + 1;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiHalfwordShuffleVectorOps++;
#endif
			return emit_load_q(rt, rt_qreg, HOST_TMP0) &&
				   m_code.EmitVdupI16D(rt_low_d, rt_low_d, 0) &&
				   m_code.EmitVdupI16D(rt_high_d, rt_high_d, 0) &&
				   emit_store_rd_from_q(rt_qreg);
		}

		const auto emit_load_gpr_word = [this](unsigned guest_reg, unsigned word, unsigned host_reg) {
			return EmitLoadGprWord(guest_reg, word, host_reg);
		};

		const auto emit_store_rd_word = [this, rd](unsigned word, unsigned host_reg) {
			return EmitStoreGprWord(rd, word, host_reg);
		};

		const auto emit_pack = [this](unsigned low_reg, bool low_high_half, unsigned high_reg,
			bool high_high_half, unsigned out_reg) {
			if (low_high_half &&
				!m_code.EmitMovRegShiftImm(out_reg, low_reg, VitaA32::ShiftType::LSR, 16))
			{
				return false;
			}

			const unsigned rn = low_high_half ? out_reg : low_reg;
			const u8 high_shift = high_high_half ? 0 : 16;
			return m_code.EmitPkhbt(out_reg, rn, high_reg, high_shift);
		};

		const auto emit_pin_word = [&](unsigned out_word, unsigned rt_word, unsigned rs_word) {
			return emit_load_gpr_word(rt, rt_word, HOST_TMP0) &&
				   emit_load_gpr_word(rs, rs_word, HOST_TMP1) &&
				   emit_pack(HOST_TMP0, false, HOST_TMP1, false, HOST_TMP4) &&
				   emit_store_rd_word(out_word, HOST_TMP4);
		};

		switch (operation)
		{
			case MmiHalfwordShuffleOp::Pinth:
				if (!emit_load_gpr_word(rt, 0, HOST_TMP0) ||
					!emit_load_gpr_word(rs, 2, HOST_TMP1) ||
					!emit_load_gpr_word(rt, 1, HOST_TMP2) ||
					!emit_load_gpr_word(rs, 3, HOST_TMP3))
	{
					return false;
	}
				return emit_pack(HOST_TMP0, false, HOST_TMP1, false, HOST_TMP4) &&
					   emit_store_rd_word(0, HOST_TMP4) &&
					   emit_pack(HOST_TMP0, true, HOST_TMP1, true, HOST_TMP4) &&
					   emit_store_rd_word(1, HOST_TMP4) &&
					   emit_pack(HOST_TMP2, false, HOST_TMP3, false, HOST_TMP4) &&
					   emit_store_rd_word(2, HOST_TMP4) &&
					   emit_pack(HOST_TMP2, true, HOST_TMP3, true, HOST_TMP4) &&
					   emit_store_rd_word(3, HOST_TMP4);
			case MmiHalfwordShuffleOp::Pinteh:
				return emit_pin_word(0, 0, 0) &&
					   emit_pin_word(1, 1, 1) &&
					   emit_pin_word(2, 2, 2) &&
					   emit_pin_word(3, 3, 3);
			case MmiHalfwordShuffleOp::Pexeh:
				return emit_load_gpr_word(rt, 0, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 1, HOST_TMP1) &&
					   emit_pack(HOST_TMP1, false, HOST_TMP0, true, HOST_TMP4) &&
					   emit_store_rd_word(0, HOST_TMP4) &&
					   emit_pack(HOST_TMP0, false, HOST_TMP1, true, HOST_TMP4) &&
					   emit_store_rd_word(1, HOST_TMP4) &&
					   emit_load_gpr_word(rt, 2, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 3, HOST_TMP1) &&
					   emit_pack(HOST_TMP1, false, HOST_TMP0, true, HOST_TMP4) &&
					   emit_store_rd_word(2, HOST_TMP4) &&
					   emit_pack(HOST_TMP0, false, HOST_TMP1, true, HOST_TMP4) &&
					   emit_store_rd_word(3, HOST_TMP4);
			case MmiHalfwordShuffleOp::Prevh:
				return emit_load_gpr_word(rt, 0, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 1, HOST_TMP1) &&
					   emit_pack(HOST_TMP1, true, HOST_TMP1, false, HOST_TMP4) &&
					   emit_store_rd_word(0, HOST_TMP4) &&
					   emit_pack(HOST_TMP0, true, HOST_TMP0, false, HOST_TMP4) &&
					   emit_store_rd_word(1, HOST_TMP4) &&
					   emit_load_gpr_word(rt, 2, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 3, HOST_TMP1) &&
					   emit_pack(HOST_TMP1, true, HOST_TMP1, false, HOST_TMP4) &&
					   emit_store_rd_word(2, HOST_TMP4) &&
					   emit_pack(HOST_TMP0, true, HOST_TMP0, false, HOST_TMP4) &&
					   emit_store_rd_word(3, HOST_TMP4);
			case MmiHalfwordShuffleOp::Pexch:
				return emit_load_gpr_word(rt, 0, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 1, HOST_TMP1) &&
					   emit_pack(HOST_TMP0, false, HOST_TMP1, false, HOST_TMP4) &&
					   emit_store_rd_word(0, HOST_TMP4) &&
					   emit_pack(HOST_TMP0, true, HOST_TMP1, true, HOST_TMP4) &&
					   emit_store_rd_word(1, HOST_TMP4) &&
					   emit_load_gpr_word(rt, 2, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 3, HOST_TMP1) &&
					   emit_pack(HOST_TMP0, false, HOST_TMP1, false, HOST_TMP4) &&
					   emit_store_rd_word(2, HOST_TMP4) &&
					   emit_pack(HOST_TMP0, true, HOST_TMP1, true, HOST_TMP4) &&
					   emit_store_rd_word(3, HOST_TMP4);
			case MmiHalfwordShuffleOp::Pcpyh:
				return emit_load_gpr_word(rt, 0, HOST_TMP0) &&
					   emit_pack(HOST_TMP0, false, HOST_TMP0, false, HOST_TMP4) &&
					   emit_store_rd_word(0, HOST_TMP4) &&
					   emit_store_rd_word(1, HOST_TMP4) &&
					   emit_load_gpr_word(rt, 2, HOST_TMP0) &&
					   emit_pack(HOST_TMP0, false, HOST_TMP0, false, HOST_TMP4) &&
					   emit_store_rd_word(2, HOST_TMP4) &&
					   emit_store_rd_word(3, HOST_TMP4);
		}

		return false;
	}

	bool BlockCompiler::EmitMmiInterleaveOp(u32 op, MmiUpperInterleaveOp operation, bool upper_half)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RS = 0;
		constexpr unsigned NEON_RT = 1;

		const auto emit_interleave = [&](unsigned rt_qreg, unsigned rs_qreg) {
			switch (operation)
			{
				case MmiUpperInterleaveOp::Word:
					return m_code.EmitVzipI32Q(rt_qreg, rs_qreg);
				case MmiUpperInterleaveOp::Halfword:
					return m_code.EmitVzipI16Q(rt_qreg, rs_qreg);
				case MmiUpperInterleaveOp::Byte:
					return m_code.EmitVzipI8Q(rt_qreg, rs_qreg);
			}

			return false;
		};

		if (rs == 0 && rt == 0)
		{
			const unsigned result = upper_half ? NEON_RS : NEON_RT;
			return m_code.EmitVeorQ(result, result, result) &&
				   EmitStoreGprQ128(rd, result, HOST_TMP2);
		}

		// PCSX2 MMI.cpp::PEXTL*/PEXTU* interleave only one 64-bit half from
		// each source. Materialize those D halves directly instead of loading
		// full qwords; the other D halves are only written to the discarded
		// vzip result register.
		{
			const unsigned half_index = upper_half ? 1 : 0;
			const unsigned rt_d = NEON_RT * 2 + half_index;
			const unsigned rs_d = NEON_RS * 2 + half_index;
			const unsigned result = upper_half ? NEON_RS : NEON_RT;
			if (rs == rt)
			{
				const int cached_qreg = FindGprQCache(rs);
				bool used_qregs[MAX_GPR_QCACHE]{};
				if (cached_qreg >= 0)
					used_qregs[static_cast<unsigned>(cached_qreg)] = true;
				const auto choose_qreg = [&]() {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
						if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
						{
							used_qregs[qreg] = true;
							return qreg;
						}
	}

	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
						if (!used_qregs[qreg])
						{
							used_qregs[qreg] = true;
							return qreg;
						}
	}

					return MAX_GPR_QCACHE;
	};
	const unsigned rt_qreg = cached_qreg >= 0 ? choose_qreg() : NEON_RT;
				const unsigned rs_qreg = cached_qreg >= 0 ? choose_qreg() : NEON_RS;
				if (rt_qreg >= MAX_GPR_QCACHE || rs_qreg >= MAX_GPR_QCACHE)
					return false;

				const unsigned same_rt_d = rt_qreg * 2 + half_index;
				const unsigned same_rs_d = rs_qreg * 2 + half_index;
				const unsigned same_result = upper_half ? rs_qreg : rt_qreg;
				if (cached_qreg >= 0)
	{
					const unsigned source_d = static_cast<unsigned>(cached_qreg) * 2 + half_index;
					if (source_d != same_rt_d && !m_code.EmitVorrD(same_rt_d, source_d, source_d))
						return false;
	}
				else if (!m_code.EmitVldrDImm(rt_d, HOST_CPU_REGS,
					static_cast<u16>(GprOffset(rs) + half_index * sizeof(u64))))
	{
					return false;
	}

				InvalidateGprQCacheForQreg(rt_qreg);
				InvalidateGprQCacheForQreg(rs_qreg);
				if (!m_code.EmitVorrD(same_rs_d, same_rt_d, same_rt_d) ||
					!emit_interleave(rt_qreg, rs_qreg) ||
					!EmitStoreGprQ128(rd, same_result, HOST_TMP2))
	{
					return false;
	}

#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectInterleaveOps++;
#endif
				return true;
			}

			struct HalfSource
			{
				unsigned guest_reg;
				unsigned target_d;
				int cached_qreg = -1;
				bool emitted = false;
			};

			HalfSource rt_source{rt, rt_d};
			HalfSource rs_source{rs, rs_d};
			if (rt != 0)
				rt_source.cached_qreg = FindGprQCache(rt);
			if (rs != 0)
				rs_source.cached_qreg = FindGprQCache(rs);

			const auto choose_scratch_qreg = [&](unsigned avoid_qreg) {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (qreg != avoid_qreg && !IsGprQCacheQregResident(qreg))
						return qreg;
	}

	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (qreg != avoid_qreg)
						return qreg;
	}

				return NEON_RS;
			};

			const auto emit_direct_interleave = [&](unsigned rt_qreg, unsigned rs_qreg) {
				if (rt_qreg == rs_qreg)
					return false;

				const unsigned rt_target_d = rt_qreg * 2 + half_index;
				const unsigned rs_target_d = rs_qreg * 2 + half_index;
				const auto emit_direct_source = [&](unsigned guest_reg, unsigned target_d) {
					if (guest_reg == 0)
						return m_code.EmitVeorD(target_d, target_d, target_d);

					return m_code.EmitVldrDImm(target_d, HOST_CPU_REGS,
						static_cast<u16>(GprOffset(guest_reg) + half_index * sizeof(u64)));
	};

				if (rt_source.cached_qreg < 0 &&
					!emit_direct_source(rt, rt_target_d))
	{
					return false;
	}
				if (rs_source.cached_qreg < 0 &&
					!emit_direct_source(rs, rs_target_d))
	{
					return false;
	}

				InvalidateGprQCacheForQreg(rt_qreg);
				InvalidateGprQCacheForQreg(rs_qreg);
				if (!emit_interleave(rt_qreg, rs_qreg) ||
					!EmitStoreGprQ128(rd, upper_half ? rs_qreg : rt_qreg, HOST_TMP2))
	{
					return false;
	}

#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectInterleaveOps++;
#endif
				return true;
			};

			if (rt_source.cached_qreg >= 0 && rs_source.cached_qreg >= 0 &&
				rt_source.cached_qreg != rs_source.cached_qreg)
			{
				return emit_direct_interleave(static_cast<unsigned>(rt_source.cached_qreg),
					static_cast<unsigned>(rs_source.cached_qreg));
			}

			if (rt_source.cached_qreg >= 0)
			{
	const unsigned rt_qreg = static_cast<unsigned>(rt_source.cached_qreg);
				const unsigned rs_qreg = choose_scratch_qreg(rt_qreg);
				return emit_direct_interleave(rt_qreg, rs_qreg);
			}

			if (rs_source.cached_qreg >= 0)
			{
				const unsigned rs_qreg = static_cast<unsigned>(rs_source.cached_qreg);
	const unsigned rt_qreg = choose_scratch_qreg(rs_qreg);
				return emit_direct_interleave(rt_qreg, rs_qreg);
			}

			const auto cached_source_d = [half_index](const HalfSource& source) {
				return static_cast<unsigned>(source.cached_qreg) * 2 + half_index;
			};

			const auto emit_cached_source = [&](HalfSource& source) {
				if (source.cached_qreg < 0 || source.emitted)
					return true;

				source.emitted = true;
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDwordHits++;
#endif
				const unsigned source_d = cached_source_d(source);
				return source_d == source.target_d ||
					   m_code.EmitVorrD(source.target_d, source_d, source_d);
			};

			const auto cached_source_would_be_clobbered = [&](const HalfSource& source,
				const HalfSource& clobber) {
				return source.cached_qreg >= 0 && !source.emitted &&
					   cached_source_d(source) == clobber.target_d &&
					   cached_source_d(source) != source.target_d;
			};

			if ((cached_source_would_be_clobbered(rt_source, rs_source) &&
				 !emit_cached_source(rt_source)) ||
				(cached_source_would_be_clobbered(rs_source, rt_source) &&
				 !emit_cached_source(rs_source)) ||
				!emit_cached_source(rt_source) ||
				!emit_cached_source(rs_source))
			{
				return false;
			}

			const auto emit_uncached_source = [&](HalfSource& source) {
				if (source.emitted)
					return true;
				source.emitted = true;

				if (source.guest_reg == 0)
					return m_code.EmitVeorD(source.target_d, source.target_d, source.target_d);

				return m_code.EmitVldrDImm(source.target_d, HOST_CPU_REGS,
					static_cast<u16>(GprOffset(source.guest_reg) + half_index * sizeof(u64)));
			};

			if (!emit_uncached_source(rt_source) ||
				!emit_uncached_source(rs_source))
			{
				return false;
			}

			InvalidateGprQCacheForQreg(NEON_RS);
			InvalidateGprQCacheForQreg(NEON_RT);
			return emit_interleave(NEON_RT, NEON_RS) &&
				   EmitStoreGprQ128(rd, result, HOST_TMP2);
		}
	}

	bool BlockCompiler::EmitMmiUpperInterleaveOp(u32 op, MmiUpperInterleaveOp operation)
	{
		return EmitMmiInterleaveOp(op, operation, true);
	}

	bool BlockCompiler::EmitMmiPackEvenOp(u32 op, MmiUpperInterleaveOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RS = 0;
		constexpr unsigned NEON_RT = 1;

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			return EmitCpuRegsAddress(host_reg, GprOffset(guest_reg));
		};

		const auto emit_store_rd_from = [&](unsigned qreg) {
			if (rd == rs && rs != 0)
			{
				return emit_gpr_address(rd, HOST_TMP0) &&
					   EmitStoreGprQ128ToAddress(rd, qreg, HOST_TMP0);
			}

			if (rd == rt && rt != 0)
			{
				return emit_gpr_address(rd, HOST_TMP1) &&
					   EmitStoreGprQ128ToAddress(rd, qreg, HOST_TMP1);
			}

			return EmitStoreGprQ128(rd, qreg, HOST_TMP2);
		};

		const auto emit_pack_even = [&](unsigned rt_qreg, unsigned rs_qreg) {
			switch (operation)
			{
				case MmiUpperInterleaveOp::Word:
					return m_code.EmitVuzpI32Q(rt_qreg, rs_qreg);
				case MmiUpperInterleaveOp::Halfword:
					return m_code.EmitVuzpI16Q(rt_qreg, rs_qreg);
				case MmiUpperInterleaveOp::Byte:
					return m_code.EmitVuzpI8Q(rt_qreg, rs_qreg);
			}

			return false;
		};
		const auto should_load_single_use_entry_operand = [&](unsigned guest_reg) {
			return ShouldLoadGprQ128SingleUseEntry(guest_reg);
		};
		const auto load_qword_operand = [&](unsigned guest_reg, unsigned qreg,
										 unsigned address_scratch) {
			if (should_load_single_use_entry_operand(guest_reg))
				return EmitLoadGprQ128SingleUseEntry(guest_reg, qreg,
					address_scratch);
			return EmitLoadGprQ128(guest_reg, qreg, address_scratch);
		};

		const auto emit_pack_even_from_qregs = [&](unsigned rt_qreg, unsigned rs_qreg,
			unsigned cached_rt_guest = 0, unsigned cached_rs_guest = 0) {
			if (rt_qreg == rs_qreg)
				return false;

			if ((cached_rt_guest != 0 &&
				 !PreserveGprQCacheGuestForFutureRead(cached_rt_guest, rt_qreg,
					 rt_qreg, rs_qreg, nullptr)) ||
				(cached_rs_guest != 0 &&
				 !PreserveGprQCacheGuestForFutureRead(cached_rs_guest, rs_qreg,
					 rt_qreg, rs_qreg, nullptr)))
			{
				return false;
			}

			InvalidateGprQCacheForQreg(rs_qreg);
			InvalidateGprQCacheForQreg(rt_qreg);
			if (!emit_pack_even(rt_qreg, rs_qreg) ||
				!emit_store_rd_from(rt_qreg))
			{
				return false;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheDirectPackEvenOps++;
#endif
			return true;
		};

		const int cached_rs_qreg = FindGprQCache(rs);
		const int cached_rt_qreg = FindGprQCache(rt);
		if (rs == 0 && rt == 0)
		{
			const unsigned result_qreg = [&]() {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (!IsGprQCacheQregResident(qreg))
						return qreg;
	}

				return NEON_RT;
			}();
			InvalidateGprQCacheForQreg(result_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackEvenZeroSourceOps++;
#endif
			return m_code.EmitVeorQ(result_qreg, result_qreg, result_qreg) &&
				   EmitStoreGprQ128(rd, result_qreg, HOST_TMP2);
		}

		if (rs == rt && rs != 0)
		{
			const auto choose_source_qreg = [&]() {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (!IsGprQCacheQregResident(qreg))
						return qreg;
	}

				return NEON_RT;
			};
			const auto choose_duplicate_qreg = [&](unsigned avoid) {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (qreg != avoid && !IsGprQCacheQregResident(qreg))
						return qreg;
	}

	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (qreg != avoid)
						return qreg;
	}

				return MAX_GPR_QCACHE;
			};
			const unsigned source_qreg = cached_rs_qreg >= 0 ?
	static_cast<unsigned>(cached_rs_qreg) : choose_source_qreg();
			if (cached_rs_qreg < 0 && !load_qword_operand(rs, source_qreg, HOST_TMP0))
				return false;

			const unsigned duplicate_qreg = choose_duplicate_qreg(source_qreg);
			if (duplicate_qreg >= MAX_GPR_QCACHE)
				return false;
			InvalidateGprQCacheForQreg(duplicate_qreg);
			if (!m_code.EmitVorrQ(duplicate_qreg, source_qreg, source_qreg))
				return false;

#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheSameSourceQregReuses++;
#endif
			return emit_pack_even_from_qregs(source_qreg, duplicate_qreg,
				cached_rs_qreg >= 0 ? rs : 0, 0);
		}

		if (rs == 0 || rt == 0)
		{
			const bool rt_is_zero = rt == 0;
			const unsigned source_guest = rt_is_zero ? rs : rt;
			const int cached_source_qreg = rt_is_zero ? cached_rs_qreg : cached_rt_qreg;
			const auto choose_qreg = [&](int avoid0 = -1, int avoid1 = -1) {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
	if (static_cast<int>(qreg) != avoid0 &&
						static_cast<int>(qreg) != avoid1 &&
						!IsGprQCacheQregResident(qreg))
	{
						return qreg;
	}
	}

	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
	if (static_cast<int>(qreg) != avoid0 &&
						static_cast<int>(qreg) != avoid1)
	{
						return qreg;
	}
	}

				return MAX_GPR_QCACHE;
			};

			const unsigned source_qreg = choose_qreg(cached_source_qreg);
			const unsigned zero_qreg = choose_qreg(static_cast<int>(source_qreg),
				cached_source_qreg);
			if (source_qreg >= MAX_GPR_QCACHE || zero_qreg >= MAX_GPR_QCACHE)
				return false;

			if (cached_source_qreg >= 0)
			{
				InvalidateGprQCacheForQreg(source_qreg);
				if (!m_code.EmitVorrQ(source_qreg,
						static_cast<unsigned>(cached_source_qreg),
						static_cast<unsigned>(cached_source_qreg)))
	{
					return false;
	}
			}
			else if (!load_qword_operand(source_guest, source_qreg,
						 rt_is_zero ? HOST_TMP0 : HOST_TMP1))
			{
				return false;
			}

			InvalidateGprQCacheForQreg(zero_qreg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackEvenZeroSourceOps++;
#endif
			if (!m_code.EmitVeorQ(zero_qreg, zero_qreg, zero_qreg))
				return false;

			return rt_is_zero ?
				emit_pack_even_from_qregs(zero_qreg, source_qreg) :
				emit_pack_even_from_qregs(source_qreg, zero_qreg);
		}

		if (cached_rs_qreg >= 0 && cached_rt_qreg >= 0 &&
			cached_rs_qreg != cached_rt_qreg)
		{
			return emit_pack_even_from_qregs(static_cast<unsigned>(cached_rt_qreg),
	static_cast<unsigned>(cached_rs_qreg), rt, rs);
		}

		if (cached_rs_qreg >= 0 && rt != 0)
		{
			const unsigned rs_qreg = static_cast<unsigned>(cached_rs_qreg);
			const auto choose_qreg = [&](unsigned avoid) {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (qreg != avoid && !IsGprQCacheQregResident(qreg))
						return qreg;
	}

				return avoid == NEON_RT ? NEON_RS : NEON_RT;
			};
			const unsigned rt_qreg = choose_qreg(rs_qreg);
			if (rt_qreg != rs_qreg &&
				load_qword_operand(rt, rt_qreg, HOST_TMP1))
			{
				return emit_pack_even_from_qregs(rt_qreg, rs_qreg, 0, rs);
			}
			return false;
		}

		if (cached_rt_qreg >= 0 && rs != 0)
		{
			const unsigned rt_qreg = static_cast<unsigned>(cached_rt_qreg);
			const auto choose_qreg = [&](unsigned avoid) {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
					if (qreg != avoid && !IsGprQCacheQregResident(qreg))
						return qreg;
	}

				return avoid == NEON_RS ? NEON_RT : NEON_RS;
			};
			const unsigned rs_qreg = choose_qreg(rt_qreg);
			if (rs_qreg != rt_qreg &&
				load_qword_operand(rs, rs_qreg, HOST_TMP0))
			{
				return emit_pack_even_from_qregs(rt_qreg, rs_qreg, rt, 0);
			}
			return false;
		}

		bool used_qregs[MAX_GPR_QCACHE]{};
		const auto choose_qreg = [&]() {
			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
	{
					used_qregs[qreg] = true;
	return qreg;
	}
			}

			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg])
	{
					used_qregs[qreg] = true;
	return qreg;
	}
			}

			return MAX_GPR_QCACHE;
		};
		const unsigned rs_qreg = choose_qreg();
		const unsigned rt_qreg = choose_qreg();
		if (rs_qreg >= MAX_GPR_QCACHE || rt_qreg >= MAX_GPR_QCACHE ||
			!load_qword_operand(rs, rs_qreg, HOST_TMP0) ||
			!load_qword_operand(rt, rt_qreg, HOST_TMP1))
		{
			return false;
		}

		// vuzp RT, RS leaves the even RT lanes followed by the even RS lanes in
		// RT, matching PCSX2's PPACW/PPACH/PPACB definitions.
		return emit_pack_even_from_qregs(rt_qreg, rs_qreg);
	}

	bool BlockCompiler::EmitPABSW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PABSW(), which computes saturating absolute
		// values for four signed 32-bit lanes from RT and ignores RS.
		return EmitMmiUnaryRtVectorOp(op, MmiUnaryVectorOp::AbsoluteSignedWord);
	}

	bool BlockCompiler::EmitPSLLH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::tbl_MMI dispatches this opcode
		// to MMI.cpp::PSLLH(), which left-shifts eight 16-bit lanes by SA & 0xf.
		return EmitMmiImmediateShiftOp(op, MmiImmediateShiftOp::ShiftLeftHalfword);
	}

	bool BlockCompiler::EmitPSRLH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::tbl_MMI dispatches this opcode
		// to MMI.cpp::PSRLH(), which logical-right-shifts eight 16-bit lanes by SA & 0xf.
		return EmitMmiImmediateShiftOp(op, MmiImmediateShiftOp::ShiftRightLogicalHalfword);
	}

	bool BlockCompiler::EmitPSRAH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::tbl_MMI dispatches this opcode
		// to MMI.cpp::PSRAH(), which arithmetic-right-shifts eight signed 16-bit lanes by SA & 0xf.
		return EmitMmiImmediateShiftOp(op, MmiImmediateShiftOp::ShiftRightArithmeticHalfword);
	}

	bool BlockCompiler::EmitPSLLW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::tbl_MMI dispatches this opcode
		// to MMI.cpp::PSLLW(), which left-shifts four 32-bit lanes by SA.
		return EmitMmiImmediateShiftOp(op, MmiImmediateShiftOp::ShiftLeftWord);
	}

	bool BlockCompiler::EmitPSRLW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::tbl_MMI dispatches this opcode
		// to MMI.cpp::PSRLW(), which logical-right-shifts four 32-bit lanes by SA.
		return EmitMmiImmediateShiftOp(op, MmiImmediateShiftOp::ShiftRightLogicalWord);
	}

	bool BlockCompiler::EmitPSRAW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::tbl_MMI dispatches this opcode
		// to MMI.cpp::PSRAW(), which arithmetic-right-shifts four signed 32-bit lanes by SA.
		return EmitMmiImmediateShiftOp(op, MmiImmediateShiftOp::ShiftRightArithmeticWord);
	}

	bool BlockCompiler::EmitPADDW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PADDW(), which wraps four independent 32-bit lanes.
		// The Cortex-A9 path uses call-clobbered NEON q0-q2 as block-local
		// temporaries; this matches the full128 MMI promotion rule in the Vita
		// ARM optimization plan.
		return EmitMmiVectorOp(op, MmiVectorOp::AddWord);
	}

	bool BlockCompiler::EmitPSUBW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PSUBW(), which wraps four independent 32-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::SubtractWord);
	}

	bool BlockCompiler::EmitPCGTW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PCGTW(), signed-compares four independent 32-bit
		// lanes, and writes each result as 0xffffffff or 0x00000000.
		return EmitMmiVectorOp(op, MmiVectorOp::CompareGreaterSignedWord);
	}

	bool BlockCompiler::EmitPMAXW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PMAXW(), which writes the signed max of each
		// independent 32-bit lane.
		return EmitMmiVectorOp(op, MmiVectorOp::MaxSignedWord);
	}

	bool BlockCompiler::EmitPADDH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PADDH(), which wraps eight independent 16-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::AddHalfword);
	}

	bool BlockCompiler::EmitPSUBH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PSUBH(), which wraps eight independent 16-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::SubtractHalfword);
	}

	bool BlockCompiler::EmitPCGTH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PCGTH(), signed-compares eight independent 16-bit
		// lanes, and writes each result as 0xffff or 0x0000.
		return EmitMmiVectorOp(op, MmiVectorOp::CompareGreaterSignedHalfword);
	}

	bool BlockCompiler::EmitPMAXH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PMAXH(), which writes the signed max of each
		// independent 16-bit lane.
		return EmitMmiVectorOp(op, MmiVectorOp::MaxSignedHalfword);
	}

	bool BlockCompiler::EmitPADDB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PADDB(), which wraps sixteen independent 8-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::AddByte);
	}

	bool BlockCompiler::EmitPSUBB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PSUBB(), which wraps sixteen independent 8-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::SubtractByte);
	}

	bool BlockCompiler::EmitPCGTB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PCGTB(), signed-compares sixteen independent 8-bit
		// lanes, and writes each result as 0xff or 0x00.
		return EmitMmiVectorOp(op, MmiVectorOp::CompareGreaterSignedByte);
	}

	bool BlockCompiler::EmitPADDSW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PADDSW(), which signed-saturates four 32-bit sums.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingAddSignedWord);
	}

	bool BlockCompiler::EmitPSUBSW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PSUBSW(), which signed-saturates four 32-bit differences.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingSubtractSignedWord);
	}

	bool BlockCompiler::EmitPEXTLW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PEXTLW(), which interleaves the lower 32-bit lanes
		// of RT and RS as RT0, RS0, RT1, RS1.
		return EmitMmiInterleaveOp(op, MmiUpperInterleaveOp::Word, false);
	}

	bool BlockCompiler::EmitPPACW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PPACW(), which packs even 32-bit lanes as
		// RT0, RT2, RS0, RS2.
		return EmitMmiPackEvenOp(op, MmiUpperInterleaveOp::Word);
	}

	bool BlockCompiler::EmitPADDSH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PADDSH(), which signed-saturates eight 16-bit sums.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingAddSignedHalfword);
	}

	bool BlockCompiler::EmitPSUBSH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PSUBSH(), which signed-saturates eight 16-bit differences.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingSubtractSignedHalfword);
	}

	bool BlockCompiler::EmitPEXTLH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PEXTLH(), which interleaves the lower 16-bit lanes
		// of RT and RS as RT0, RS0, ... RT3, RS3.
		return EmitMmiInterleaveOp(op, MmiUpperInterleaveOp::Halfword, false);
	}

	bool BlockCompiler::EmitPPACH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PPACH(), which packs even 16-bit lanes as
		// RT0, RT2, RT4, RT6, RS0, RS2, RS4, RS6.
		return EmitMmiPackEvenOp(op, MmiUpperInterleaveOp::Halfword);
	}

	bool BlockCompiler::EmitPADDSB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PADDSB(), which signed-saturates sixteen 8-bit sums.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingAddSignedByte);
	}

	bool BlockCompiler::EmitPSUBSB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PSUBSB(), which signed-saturates sixteen 8-bit differences.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingSubtractSignedByte);
	}

	bool BlockCompiler::EmitPEXTLB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PEXTLB(), which interleaves the lower 8-bit lanes
		// of RT and RS as RT0, RS0, ... RT7, RS7.
		return EmitMmiInterleaveOp(op, MmiUpperInterleaveOp::Byte, false);
	}

	bool BlockCompiler::EmitPPACB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PPACB(), which packs even 8-bit lanes as
		// RT0, RT2, ... RT14, RS0, RS2, ... RS14.
		return EmitMmiPackEvenOp(op, MmiUpperInterleaveOp::Byte);
	}

	bool BlockCompiler::EmitPEXT5(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PEXT5(), which expands each RT 5:5:5:1 word into
		// byte-aligned 8:8:8:8 fields.
		return EmitMmiFiveBitOp(op, MmiFiveBitOp::Expand);
	}

	bool BlockCompiler::EmitPPAC5(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PPAC5(), which packs each RT 8:8:8:8 word back
		// into a 5:5:5:1 field layout.
		return EmitMmiFiveBitOp(op, MmiFiveBitOp::Pack);
	}

	bool BlockCompiler::EmitPCEQW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PCEQW(), which writes 0xffffffff for equal 32-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::CompareEqualWord);
	}

	bool BlockCompiler::EmitPMINW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PMINW(), which writes the signed min of each 32-bit lane.
		return EmitMmiVectorOp(op, MmiVectorOp::MinSignedWord);
	}

	bool BlockCompiler::EmitPADSBH(u32 op)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RS = 0;

		const auto choose_nonresident_qreg = [&]() {
			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!IsGprQCacheQregResident(qreg))
	return qreg;
			}

			return NEON_RS;
		};

		const auto emit_zero_qword = [&]() {
			return m_code.EmitVeorQ(NEON_RS, NEON_RS, NEON_RS) &&
				   EmitStoreGprQ128(rd, NEON_RS, HOST_TMP2);
		};

		const auto emit_copy_qword = [&]() {
			if (rs == 0)
				return emit_zero_qword();

			if (rd == rs)
				return true;

			const int cached_qreg = FindGprQCache(rs);
			if (cached_qreg >= 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectPadsbhOps++;
#endif
				return EmitStoreGprQ128(rd, static_cast<unsigned>(cached_qreg), HOST_TMP2);
			}

			const unsigned source_qreg = choose_nonresident_qreg();
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheDirectPadsbhOps++;
#endif
			return EmitLoadGprQ128(rs, source_qreg, HOST_TMP0) &&
				   EmitStoreGprQ128(rd, source_qreg, HOST_TMP2);
		};

		if (rt == 0)
			return emit_copy_qword();

		if (rs == rt)
		{
			// PCSX2 owners: MMI.cpp::PADSBH() and x86/iMMI.cpp::recPADSBH().
			// Same-source PADSBH is lower-half zero and upper-half PADDH(rs, rs).
			if (rs == 0)
				return emit_zero_qword();

			if (rd == rs)
			{
				const int cached_qreg = FindGprQCache(rs);
				if (cached_qreg >= 0)
	{
					const unsigned source_qreg = static_cast<unsigned>(cached_qreg);
					InvalidateGprQCacheForGuest(rs);
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuGprQCacheDirectPadsbhOps++;
#endif
					return m_code.EmitVaddI16Q(source_qreg, source_qreg, source_qreg) &&
					       m_code.EmitVeorD(source_qreg * 2, source_qreg * 2, source_qreg * 2) &&
					       EmitStoreGprQ128(rd, source_qreg, HOST_TMP2);
	}
			}

			const unsigned source_qreg = choose_nonresident_qreg();
			if (!EmitLoadGprQ128(rs, source_qreg, HOST_TMP0))
				return false;

			InvalidateGprQCacheForGuest(rs);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheDirectPadsbhOps++;
#endif
			return m_code.EmitVaddI16Q(source_qreg, source_qreg, source_qreg) &&
				   m_code.EmitVeorD(source_qreg * 2, source_qreg * 2, source_qreg * 2) &&
				   EmitStoreGprQ128(rd, source_qreg, HOST_TMP2);
		}

		const int cached_rs_qreg = FindGprQCache(rs);
		const int cached_rt_qreg = FindGprQCache(rt);
		bool used_qregs[MAX_GPR_QCACHE]{};
		unsigned rs_qreg = 0;
		unsigned rt_qreg = 0;
		const auto choose_qreg = [&]() {
			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
	{
					used_qregs[qreg] = true;
	return qreg;
	}
			}

			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg])
	{
					used_qregs[qreg] = true;
	return qreg;
	}
			}

			return MAX_GPR_QCACHE;
		};

		if (cached_rs_qreg >= 0)
		{
			rs_qreg = static_cast<unsigned>(cached_rs_qreg);
			used_qregs[rs_qreg] = true;
		}
		if (cached_rt_qreg >= 0)
		{
			rt_qreg = static_cast<unsigned>(cached_rt_qreg);
			used_qregs[rt_qreg] = true;
		}
		if (cached_rs_qreg < 0)
		{
			rs_qreg = choose_qreg();
			if (rs_qreg >= MAX_GPR_QCACHE || !EmitLoadGprQ128(rs, rs_qreg, HOST_TMP0))
				return false;
		}
		if (cached_rt_qreg < 0)
		{
			rt_qreg = choose_qreg();
			if (rt_qreg >= MAX_GPR_QCACHE || !EmitLoadGprQ128(rt, rt_qreg, HOST_TMP1))
				return false;
		}

		const unsigned sub_qreg = choose_qreg();
		const unsigned add_qreg = choose_qreg();
		if (sub_qreg >= MAX_GPR_QCACHE || add_qreg >= MAX_GPR_QCACHE)
			return false;

		// PCSX2 owner: MMI.cpp::PADSBH() subtracts lanes 0..3 with PSUBH
		// semantics and adds lanes 4..7 with PADDH semantics.
		InvalidateGprQCacheForQreg(sub_qreg);
		InvalidateGprQCacheForQreg(add_qreg);
		if (!m_code.EmitVsubI16Q(sub_qreg, rs_qreg, rt_qreg) ||
			!m_code.EmitVaddI16Q(add_qreg, rs_qreg, rt_qreg) ||
			!EmitStoreGprDwordPair(rd, sub_qreg * 2, add_qreg * 2 + 1))
		{
			return false;
		}

#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprQCacheDirectPadsbhOps++;
#endif
		return true;
	}

	bool BlockCompiler::EmitPABSH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PABSH(), which computes saturating absolute
		// values for eight signed 16-bit lanes from RT and ignores RS.
		return EmitMmiUnaryRtVectorOp(op, MmiUnaryVectorOp::AbsoluteSignedHalfword);
	}

	bool BlockCompiler::EmitPCEQH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PCEQH(), which writes 0xffff for equal 16-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::CompareEqualHalfword);
	}

	bool BlockCompiler::EmitPMINH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PMINH(), which writes the signed min of each 16-bit lane.
		return EmitMmiVectorOp(op, MmiVectorOp::MinSignedHalfword);
	}

	bool BlockCompiler::EmitPCEQB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PCEQB(), which writes 0xff for equal 8-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::CompareEqualByte);
	}

	bool BlockCompiler::EmitPADDUW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PADDUW(), which unsigned-saturates four 32-bit sums.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingAddUnsignedWord);
	}

	bool BlockCompiler::EmitPSUBUW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PSUBUW(), which unsigned-saturates four 32-bit differences.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingSubtractUnsignedWord);
	}

	bool BlockCompiler::EmitPEXTUW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PEXTUW(), which interleaves the upper 32-bit lanes
		// of RT and RS as RT2, RS2, RT3, RS3.
		return EmitMmiUpperInterleaveOp(op, MmiUpperInterleaveOp::Word);
	}

	bool BlockCompiler::EmitPADDUH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PADDUH(), which unsigned-saturates eight 16-bit sums.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingAddUnsignedHalfword);
	}

	bool BlockCompiler::EmitPSUBUH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PSUBUH(), which unsigned-saturates eight 16-bit differences.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingSubtractUnsignedHalfword);
	}

	bool BlockCompiler::EmitPEXTUH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PEXTUH(), which interleaves the upper 16-bit lanes
		// of RT and RS as RT4, RS4, ... RT7, RS7.
		return EmitMmiUpperInterleaveOp(op, MmiUpperInterleaveOp::Halfword);
	}

	bool BlockCompiler::EmitPADDUB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PADDUB(), which unsigned-saturates sixteen 8-bit sums.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingAddUnsignedByte);
	}

	bool BlockCompiler::EmitPSUBUB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PSUBUB(), which unsigned-saturates sixteen 8-bit differences.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingSubtractUnsignedByte);
	}

	bool BlockCompiler::EmitPEXTUB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PEXTUB(), which interleaves the upper 8-bit lanes
		// of RT and RS as RT8, RS8, ... RT15, RS15.
		return EmitMmiUpperInterleaveOp(op, MmiUpperInterleaveOp::Byte);
	}

	bool BlockCompiler::EmitQFSRV(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::QFSRV(); x86/iMMI.cpp::recQFSRV() implements it as
		// a 16-byte unaligned extract from RT||RS at byte offset cpuRegs.sa.
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RS = 0;
		constexpr unsigned NEON_RT = 1;
		constexpr unsigned NEON_RD = 2;

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			return EmitCpuRegsAddress(host_reg, GprOffset(guest_reg));
		};

		if (rt != 0 && rs == rt + 1)
		{
			// PCSX2 x86 owner: x86/iMMI.cpp::recQFSRV() directly loads from
			// contiguous RT||RS storage when RS physically follows RT.
			if (!emit_gpr_address(rt, HOST_TMP0) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(SA_OFFSET)) ||
				!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP1, 0x0f) ||
				!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitVld1Q32(NEON_RD, HOST_TMP0) ||
				!EmitStoreGprQ128(rd, NEON_RD, HOST_TMP2))
			{
				return false;
			}

			return true;
		}

		if (m_sa_const_known)
		{
			if (m_sa_const_byte_offset == 0)
			{
				// PCSX2 MMI.cpp::QFSRV() copies RT directly when cpuRegs.sa is zero.
				if (rt == 0)
	{
					InvalidateGprQCacheForQreg(NEON_RD);
					if (!m_code.EmitVeorQ(NEON_RD, NEON_RD, NEON_RD) ||
						!EmitStoreGprQ128(rd, NEON_RD, HOST_TMP2))
	{
						return false;
	}
	}
				else if (rt != rd)
	{
					const int cached_rt_qreg = FindGprQCache(rt);
					if (cached_rt_qreg >= 0)
	{
						if (!EmitStoreGprQ128(rd, static_cast<unsigned>(cached_rt_qreg), HOST_TMP2))
							return false;
	}
					else
	{
						unsigned rt_qreg = NEON_RT;
						for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
						{
							if (!IsGprQCacheQregResident(qreg))
							{
								rt_qreg = qreg;
								break;
							}
						}

#if defined(VITASX2_QEMU_VALIDATION)
						if (rt_qreg != NEON_RT)
							g_qemuMmiQfsrvKnownSaZeroCopyQCacheOps++;
#endif
						if (!EmitLoadGprQ128(rt, rt_qreg, HOST_TMP1) ||
							!EmitStoreGprQ128(rd, rt_qreg, HOST_TMP2))
						{
							return false;
						}
	}
	}

#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuMmiQfsrvKnownSaZeroCopies++;
#endif
				return true;
			}

			const int cached_rs_qreg = rs == 0 ? -1 : FindGprQCache(rs);
			const int cached_rt_qreg = rt == 0 ? -1 : FindGprQCache(rt);
			const auto choose_qreg = [&](int avoid0, int avoid1, int avoid2) {
	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
	if (static_cast<int>(qreg) != avoid0 &&
						static_cast<int>(qreg) != avoid1 &&
						static_cast<int>(qreg) != avoid2 &&
						!IsGprQCacheQregResident(qreg))
	{
						return qreg;
	}
	}

	for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
	{
	if (static_cast<int>(qreg) != avoid0 &&
						static_cast<int>(qreg) != avoid1 &&
						static_cast<int>(qreg) != avoid2)
	{
						return qreg;
	}
	}

				return NEON_RD;
			};
			const unsigned result_qreg = choose_qreg(cached_rs_qreg, cached_rt_qreg, -1);
			const unsigned rs_qreg = cached_rs_qreg >= 0 ?
									 static_cast<unsigned>(cached_rs_qreg) :
									 choose_qreg(static_cast<int>(result_qreg), cached_rt_qreg, -1);
			const unsigned rt_qreg = cached_rt_qreg >= 0 ?
									 static_cast<unsigned>(cached_rt_qreg) :
									 choose_qreg(static_cast<int>(result_qreg), static_cast<int>(rs_qreg), -1);
			const auto emit_qfsrv_source = [&](unsigned guest_reg, int cached_qreg,
											   unsigned qreg, unsigned address_scratch) {
				if (cached_qreg >= 0)
					return true;

				InvalidateGprQCacheForQreg(qreg);
				if (guest_reg == 0)
					return m_code.EmitVeorQ(qreg, qreg, qreg);

				return EmitLoadGprQ128(guest_reg, qreg, address_scratch);
			};
			if (!emit_qfsrv_source(rs, cached_rs_qreg, rs_qreg, HOST_TMP0) ||
				!emit_qfsrv_source(rt, cached_rt_qreg, rt_qreg, HOST_TMP1))
			{
				return false;
			}

			// PCSX2 x86/iMMI.cpp::recQFSRV() extracts from RT||RS. When
			// MTSAB/MTSAH/MTSA proved cpuRegs.sa in this block, NEON vext
			// performs the same byte-window extract without the tempqw spill, and
			// resident qcached sources can feed vext directly.
			InvalidateGprQCacheForQreg(result_qreg);
			if (!m_code.EmitVextI8Q(result_qreg, rt_qreg, rs_qreg, m_sa_const_byte_offset) ||
				!EmitStoreGprQ128(rd, result_qreg, HOST_TMP2))
			{
				return false;
			}

#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiQfsrvKnownSaVectorOps++;
			if (cached_rs_qreg >= 0 || cached_rt_qreg >= 0)
				g_qemuMmiQfsrvKnownSaQCacheOps++;
#endif
			return true;
		}

		bool used_qregs[MAX_GPR_QCACHE]{};
		const auto mark_used = [&](unsigned qreg) {
			if (qreg < MAX_GPR_QCACHE)
				used_qregs[qreg] = true;
		};
		const auto choose_qreg = [&]() {
			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg] && !IsGprQCacheQregResident(qreg))
	{
					used_qregs[qreg] = true;
	return qreg;
	}
			}

			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!used_qregs[qreg])
	{
					used_qregs[qreg] = true;
	return qreg;
	}
			}

			return MAX_GPR_QCACHE;
		};

		const int cached_rs_qreg = rs == 0 ? -1 : FindGprQCache(rs);
		const int cached_rt_qreg = rt == 0 ? -1 : FindGprQCache(rt);
		unsigned rs_qreg = cached_rs_qreg >= 0 ? static_cast<unsigned>(cached_rs_qreg) : MAX_GPR_QCACHE;
		unsigned rt_qreg = cached_rt_qreg >= 0 ? static_cast<unsigned>(cached_rt_qreg) : MAX_GPR_QCACHE;
		mark_used(rs_qreg);
		mark_used(rt_qreg);
		const unsigned result_qreg = choose_qreg();
		if (result_qreg >= MAX_GPR_QCACHE)
			return false;
		if (cached_rs_qreg < 0)
		{
			rs_qreg = choose_qreg();
			if (rs_qreg >= MAX_GPR_QCACHE || !EmitLoadGprQ128(rs, rs_qreg, HOST_TMP0))
				return false;
		}
		if (cached_rt_qreg < 0)
		{
			rt_qreg = choose_qreg();
			if (rt_qreg >= MAX_GPR_QCACHE || !EmitLoadGprQ128(rt, rt_qreg, HOST_TMP1))
				return false;
		}

		// PCSX2 x86 owner: x86/iMMI.cpp::recQFSRV() spills RT||RS to tempqw
		// for non-contiguous registers, then performs one unaligned 16-byte load.
		return m_code.EmitSubImm8(HOST_SP, HOST_SP, 32) &&
			   m_code.EmitVst1Q32(rt_qreg, HOST_SP) &&
			   m_code.EmitAddImm8(HOST_TMP0, HOST_SP, 16) &&
			   m_code.EmitVst1Q32(rs_qreg, HOST_TMP0) &&
			   m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(SA_OFFSET)) &&
			   m_code.EmitAndImm8(HOST_TMP1, HOST_TMP1, 0x0f) &&
			   m_code.EmitAddReg(HOST_TMP0, HOST_SP, HOST_TMP1) &&
			   m_code.EmitVld1Q32(result_qreg, HOST_TMP0) &&
			   m_code.EmitAddImm8(HOST_SP, HOST_SP, 32) &&
			   EmitStoreGprQ128(rd, result_qreg, HOST_TMP2);
	}

	bool BlockCompiler::EmitPSLLVW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PSLLVW(), which shifts RT.UL[0]/[2] by
		// RS.UL[0]/[2] & 0x1f and sign-extends each 32-bit result to 64 bits.
		return EmitMmiVariableWordShiftOp(op, MmiVariableWordShiftOp::ShiftLeftLogical);
	}

	bool BlockCompiler::EmitPSRLVW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PSRLVW(), which logically shifts RT.UL[0]/[2] by
		// RS.UL[0]/[2] & 0x1f and sign-extends each 32-bit result to 64 bits.
		return EmitMmiVariableWordShiftOp(op, MmiVariableWordShiftOp::ShiftRightLogical);
	}

	bool BlockCompiler::EmitPSRAVW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PSRAVW(), which arithmetically shifts RT.SL[0]/[2]
		// by RS.UL[0]/[2] & 0x1f and sign-extends each result to 64 bits.
		return EmitMmiVariableWordShiftOp(op, MmiVariableWordShiftOp::ShiftRightArithmetic);
	}

	bool BlockCompiler::EmitPINTH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PINTH(), which interleaves RT low halfwords with
		// RS upper halfwords.
		return EmitMmiHalfwordShuffleOp(op, MmiHalfwordShuffleOp::Pinth);
	}

	bool BlockCompiler::EmitPCPYLD(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PCPYLD(), which copies RT.UD[0] into RD.UD[0]
		// and RS.UD[0] into RD.UD[1].
		return EmitMmiWordShuffleOp(op, MmiWordShuffleOp::Pcpyld);
	}

	bool BlockCompiler::EmitPEXEH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PEXEH(), which swaps halfwords 0/2 inside each
		// 64-bit half while leaving halfwords 1/3 in place.
		return EmitMmiHalfwordShuffleOp(op, MmiHalfwordShuffleOp::Pexeh);
	}

	bool BlockCompiler::EmitPREVH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PREVH(), which reverses the four halfwords inside
		// each 64-bit half.
		return EmitMmiHalfwordShuffleOp(op, MmiHalfwordShuffleOp::Prevh);
	}

	bool BlockCompiler::EmitPEXEW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PEXEW(), which writes RT words as 2,1,0,3.
		return EmitMmiWordShuffleOp(op, MmiWordShuffleOp::Pexew);
	}

	bool BlockCompiler::EmitPROT3W(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PROT3W(), which writes RT words as 1,2,0,3.
		return EmitMmiWordShuffleOp(op, MmiWordShuffleOp::Prot3w);
	}

	bool BlockCompiler::EmitPINTEH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PINTEH(), which interleaves even RT and RS
		// halfwords.
		return EmitMmiHalfwordShuffleOp(op, MmiHalfwordShuffleOp::Pinteh);
	}

	bool BlockCompiler::EmitPCPYUD(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PCPYUD(), which copies RS.UD[1] into RD.UD[0]
		// and RT.UD[1] into RD.UD[1].
		return EmitMmiWordShuffleOp(op, MmiWordShuffleOp::Pcpyud);
	}

	bool BlockCompiler::EmitPMFHI(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMFHI(), which copies the full 128-bit HI value
		// into RD unless RD is zero.
		return EmitMoveFullFromHiLo(op, HI_OFFSET);
	}

	bool BlockCompiler::EmitPMFLO(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMFLO(), which copies the full 128-bit LO value
		// into RD unless RD is zero.
		return EmitMoveFullFromHiLo(op, LO_OFFSET);
	}

	bool BlockCompiler::EmitPMTHI(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PMTHI(), which copies the full 128-bit RS value
		// into HI.
		return EmitMoveFullToHiLo(op, HI_OFFSET);
	}

	bool BlockCompiler::EmitPMTLO(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PMTLO(), which copies the full 128-bit RS value
		// into LO.
		return EmitMoveFullToHiLo(op, LO_OFFSET);
	}

	bool BlockCompiler::EmitPEXCH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PEXCH(), which swaps halfwords 1/2 inside each
		// 64-bit half.
		return EmitMmiHalfwordShuffleOp(op, MmiHalfwordShuffleOp::Pexch);
	}

	bool BlockCompiler::EmitPCPYH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PCPYH(), which broadcasts RT.US[0] into the lower
		// 64-bit half and RT.US[4] into the upper 64-bit half.
		return EmitMmiHalfwordShuffleOp(op, MmiHalfwordShuffleOp::Pcpyh);
	}

	bool BlockCompiler::EmitPEXCW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PEXCW(), which writes RT words as 0,2,1,3.
		return EmitMmiWordShuffleOp(op, MmiWordShuffleOp::Pexcw);
	}

	bool BlockCompiler::EmitPAND(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PAND(), which bitwise-ANDs both 64-bit halves of
		// the full 128-bit GPR value.
		return EmitMmiVectorOp(op, MmiVectorOp::BitwiseAnd);
	}

	bool BlockCompiler::EmitPXOR(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PXOR(), which bitwise-XORs both 64-bit halves of
		// the full 128-bit GPR value.
		return EmitMmiVectorOp(op, MmiVectorOp::BitwiseXor);
	}

	bool BlockCompiler::EmitPOR(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::POR(), which bitwise-ORs both 64-bit halves of
		// the full 128-bit GPR value.
		return EmitMmiVectorOp(op, MmiVectorOp::BitwiseOr);
	}

	bool BlockCompiler::EmitPNOR(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PNOR(), which writes the full 128-bit bitwise-NOR
		// of RS and RT.
		return EmitMmiVectorOp(op, MmiVectorOp::BitwiseNor);
	}

	bool BlockCompiler::EmitREGIMM(u32 op, u32 pc)
	{
		const unsigned rt = RT(op);
		const bool link = (rt == 0x10 || rt == 0x11 || rt == 0x12 || rt == 0x13);
		if (link)
		{
			// PCSX2 owners: Interpreter.cpp::BLTZAL()/BGEZAL()/BLTZALL()/BGEZALL() apply
			// R5900.h::_SetLink(31) before testing the branch condition.
			if (!EmitLink(31, pc))
				return false;
		}

		switch (rt)
		{
			case 0x00: // BLTZ, owned by Interpreter.cpp::BLTZ().
			case 0x02: // BLTZL, owned by Interpreter.cpp::BLTZL().
			case 0x10: // BLTZAL, owned by Interpreter.cpp::BLTZAL().
			case 0x12: // BLTZALL, owned by Interpreter.cpp::BLTZALL().
				return EmitBranchSigned(op, SignedBranchCondition::LessThanZero);
			case 0x01: // BGEZ, owned by Interpreter.cpp::BGEZ().
			case 0x03: // BGEZL, owned by Interpreter.cpp::BGEZL().
			case 0x11: // BGEZAL, owned by Interpreter.cpp::BGEZAL().
			case 0x13: // BGEZALL, owned by Interpreter.cpp::BGEZALL().
				return EmitBranchSigned(op, SignedBranchCondition::GreaterEqualZero);
			case 0x18: // MTSAB, owned by R5900OpcodeImpl.cpp::MTSAB().
				return EmitMTSAB(op);
			case 0x19: // MTSAH, owned by R5900OpcodeImpl.cpp::MTSAH().
				return EmitMTSAH(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitMTSAB(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MTSAB().
		const u8 imm = static_cast<u8>(IMM_U(op) & 0x0f);
		u32 value = 0;
		if (TryGetKnownGprLow(RS(op), &value))
		{
			m_sa_const_known = true;
			m_sa_const_byte_offset = static_cast<u8>((value & 0x0f) ^ imm);
		}
		else
		{
			ClearSaConstState();
		}

		unsigned rs_host;
		if (!EmitGprLowOperand(RS(op), HOST_TMP0, &rs_host) ||
			!m_code.EmitAndImm8(HOST_TMP0, rs_host, 0x0f))
		{
			return false;
		}

		if (imm != 0 && !m_code.EmitEorImm8(HOST_TMP0, HOST_TMP0, imm))
			return false;

		return m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(SA_OFFSET));
	}

	bool BlockCompiler::EmitMTSAH(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MTSAH().
		const u8 imm = static_cast<u8>(IMM_U(op) & 0x07);
		u32 value = 0;
		if (TryGetKnownGprLow(RS(op), &value))
		{
			m_sa_const_known = true;
			m_sa_const_byte_offset = static_cast<u8>(((value & 0x07) ^ imm) << 1);
		}
		else
		{
			ClearSaConstState();
		}

		unsigned rs_host;
		if (!EmitGprLowOperand(RS(op), HOST_TMP0, &rs_host) ||
			!m_code.EmitAndImm8(HOST_TMP0, rs_host, 0x07))
		{
			return false;
		}

		if (imm != 0 && !m_code.EmitEorImm8(HOST_TMP0, HOST_TMP0, imm))
			return false;

		return m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL, 1) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(SA_OFFSET));
	}

	bool BlockCompiler::EmitJ(u32, u32 pc)
	{
		return EmitJump(pc, false);
	}

	bool BlockCompiler::EmitJAL(u32, u32 pc)
	{
		return EmitJump(pc, true);
	}

	bool BlockCompiler::EmitJR(u32 op, u32 pc)
	{
		return EmitRegisterJump(op, pc, false);
	}

	bool BlockCompiler::EmitJALR(u32 op, u32 pc)
	{
		return EmitRegisterJump(op, pc, true);
	}

	bool BlockCompiler::EmitBEQ(u32 op)
	{
		return EmitBranchEqual(op, true);
	}

	bool BlockCompiler::EmitBNE(u32 op)
	{
		return EmitBranchEqual(op, false);
	}

	bool BlockCompiler::EmitBLEZ(u32 op)
	{
		return EmitBranchSigned(op, SignedBranchCondition::LessEqualZero);
	}

	bool BlockCompiler::EmitBGTZ(u32 op)
	{
		return EmitBranchSigned(op, SignedBranchCondition::GreaterThanZero);
	}

	bool BlockCompiler::EmitBEQL(u32 op)
	{
		return EmitBranchEqual(op, true);
	}

	bool BlockCompiler::EmitBNEL(u32 op)
	{
		return EmitBranchEqual(op, false);
	}

	bool BlockCompiler::EmitBLEZL(u32 op)
	{
		return EmitBranchSigned(op, SignedBranchCondition::LessEqualZero);
	}

	bool BlockCompiler::EmitBGTZL(u32 op)
	{
		return EmitBranchSigned(op, SignedBranchCondition::GreaterThanZero);
	}

	bool BlockCompiler::EmitLB(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&memRead8), true, branch_delay_slot,
			ScalarLoadWidth::Byte, 0, true);
	}

	bool BlockCompiler::EmitLH(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&memRead16), true, branch_delay_slot,
			ScalarLoadWidth::Halfword, 1, true);
	}

	bool BlockCompiler::EmitLW(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&memRead32), true, branch_delay_slot,
			ScalarLoadWidth::Word, 3, true);
	}

	bool BlockCompiler::EmitLBU(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&memRead8), false, branch_delay_slot,
			ScalarLoadWidth::Byte, 0, true);
	}

	bool BlockCompiler::EmitLHU(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&memRead16), false, branch_delay_slot,
			ScalarLoadWidth::Halfword, 1, true);
	}

	bool BlockCompiler::EmitLWU(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&memRead32), false, false,
			ScalarLoadWidth::Word, 3, false);
	}

	bool BlockCompiler::EmitLWL(u32 op)
	{
		return EmitPartialWordLoad(op, true);
	}

	bool BlockCompiler::EmitLWR(u32 op)
	{
		return EmitPartialWordLoad(op, false);
	}

	bool BlockCompiler::EmitLD(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&memRead64), false, false,
			ScalarLoadWidth::Dword, 7, false);
	}

	bool BlockCompiler::EmitLDL(u32 op)
	{
		return EmitPartialDwordLoad(op, true);
	}

	bool BlockCompiler::EmitLDR(u32 op)
	{
		return EmitPartialDwordLoad(op, false);
	}

	bool BlockCompiler::EmitLQ(u32 op)
	{
		const unsigned rt = RT(op);
		constexpr unsigned NEON_VALUE = 0;

		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address & ~0x0fu, HOST_TMP0,
				KnownVtlbFastPathKind::Qword))
		{
			if (rt == 0)
				return true;

			return m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP0) &&
				   EmitStoreGprQ128(rt, NEON_VALUE, HOST_TMP1);
		}

		size_t handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks dirty_pins;
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!EmitAlignQwordAddress(HOST_TMP0, HOST_TMP1) ||
			!EmitVtlbNonHandlerHostAddress128(
				HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback, &dirty_pins))
		{
			return false;
		}

		if (rt != 0 &&
			(!m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP0) ||
			 !EmitStoreGprQ128(rt, NEON_VALUE, HOST_TMP1)))
		{
			return false;
		}

		m_qword_load_cold_tails.push_back({
			handler_fallback,
			m_code.Size(),
			rt,
			dirty_pins,
		});
		return true;
	}

	bool BlockCompiler::EmitLWC1(u32 op)
	{
		const unsigned rt = RT(op);

		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			(known_address & 3u) == 0 &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address, HOST_TMP0,
				KnownVtlbFastPathKind::Cop1))
		{
			return m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0) &&
				   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(rt)));
		}

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks dirty_pins;
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP0, 3, true))
		{
			return false;
		}

		unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (unaligned_fallback == static_cast<size_t>(-1))
			return false;

		if (!EmitVtlbNonHandlerHostAddress(
				HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback, &dirty_pins) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(rt))))
		{
			return false;
		}

		m_cop1_word_memory_cold_tails.push_back({
			unaligned_fallback,
			handler_fallback,
			m_code.Size(),
			rt,
			false,
			dirty_pins,
		});
		return true;
	}

	bool BlockCompiler::EmitLQC2(u32 op)
	{
		const unsigned rt = RT(op);
		constexpr unsigned NEON_VALUE = 0;
		const auto emit_zero_load_skip_counter = []() -> bool {
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuCop2QwordZeroLoadSkips++;
#endif
			return true;
		};
		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address, HOST_TMP0,
				KnownVtlbFastPathKind::Cop2))
		{
			if (!EmitVu0SyncIfRunning(HOST_TMP0, HOST_TMP5))
				return false;
			if (rt == 0)
				return emit_zero_load_skip_counter();

			return m_code.EmitVld1Q32(NEON_VALUE, HOST_TMP0) &&
				   EmitVu0VfAddress(HOST_TMP0, rt) &&
				   m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP0);
		}

		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!EmitVtlbNonHandlerHostAddress128(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!EmitVu0SyncIfRunning(HOST_TMP0, HOST_TMP5))
		{
			return false;
		}

		if (rt == 0)
		{
			if (!emit_zero_load_skip_counter())
				return false;

			m_cop2_qword_memory_cold_tails.push_back({
				handler_fallback,
				m_code.Size(),
				rt,
				false,
			});
			return true;
		}

		if (!m_code.EmitVld1Q32(NEON_VALUE, HOST_TMP0))
			return false;

		if (rt != 0 &&
			(!EmitVu0VfAddress(HOST_TMP0, rt) ||
			 !m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP0)))
		{
			return false;
		}

		m_cop2_qword_memory_cold_tails.push_back({
			handler_fallback,
			m_code.Size(),
			rt,
			false,
		});
		return true;
	}

	bool BlockCompiler::EmitSB(u32 op)
	{
		const unsigned rt = RT(op);

		const auto emit_store_to_host = [&]() -> bool
		{
			const int cached_qreg = FindGprQCache(rt);
			if (cached_qreg >= 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectMemoryStores++;
#endif
				return m_code.EmitVst1D8Lane0(static_cast<unsigned>(cached_qreg) * 2, HOST_TMP0);
			}

			unsigned rt_host;
			return EmitGprLowValueOperand(rt, HOST_TMP1, &rt_host) &&
				   m_code.EmitStrbImm12(rt_host, HOST_TMP0, 0);
		};

		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address, HOST_TMP0))
		{
			return emit_store_to_host();
		}

		size_t handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks dirty_pins;
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!EmitVtlbNonHandlerHostAddress(
				HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback, &dirty_pins) ||
			!emit_store_to_host())
		{
			return false;
		}

		ScalarStoreColdTail tail{
			static_cast<size_t>(-1),
			handler_fallback,
			m_code.Size(),
			0,
			0,
			nullptr,
			reinterpret_cast<const void*>(&memWrite8),
			rt,
			ScalarStoreWidth::Byte,
		};
		tail.dirty_pins = dirty_pins;
		CaptureScalarStoreValue(&tail);
		m_scalar_store_cold_tails.push_back(tail);
		return true;
	}

	bool BlockCompiler::EmitSH(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		const unsigned rt = RT(op);

		const auto emit_store_to_host = [&]() -> bool
		{
			const int cached_qreg = FindGprQCache(rt);
			if (cached_qreg >= 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectMemoryStores++;
#endif
				return m_code.EmitVst1D16Lane0(static_cast<unsigned>(cached_qreg) * 2, HOST_TMP0);
			}

			unsigned rt_host;
			return EmitGprLowValueOperand(rt, HOST_TMP1, &rt_host) &&
				   m_code.EmitStrhImm8(rt_host, HOST_TMP0, 0);
		};

		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			(known_address & 1u) == 0 &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address, HOST_TMP0))
		{
			return emit_store_to_host();
		}

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks dirty_pins;
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP0, 1, true))
		{
			return false;
		}

		unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (unaligned_fallback == static_cast<size_t>(-1))
			return false;

		if (!EmitVtlbNonHandlerHostAddress(
				HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback, &dirty_pins) ||
			!emit_store_to_host())
		{
			return false;
		}

		ScalarStoreColdTail tail{
			unaligned_fallback,
			handler_fallback,
			m_code.Size(),
			pc,
			raw_cycles_through_instruction,
			event_exit,
			reinterpret_cast<const void*>(&memWrite16),
			rt,
			ScalarStoreWidth::Halfword,
		};
		tail.dirty_pins = dirty_pins;
		CaptureScalarStoreValue(&tail);
		m_scalar_store_cold_tails.push_back(tail);
		return true;
	}

	bool BlockCompiler::EmitSW(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		const unsigned rt = RT(op);

		const auto emit_store_to_host = [&]() -> bool
		{
			const int cached_qreg = FindGprQCache(rt);
			if (cached_qreg >= 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectMemoryStores++;
#endif
				return EmitStoreQWordLane(static_cast<unsigned>(cached_qreg), 0,
					HOST_TMP0, 0, HOST_TMP1);
			}

			unsigned rt_host;
			return EmitGprLowValueOperand(rt, HOST_TMP1, &rt_host) &&
				   m_code.EmitStrImm12(rt_host, HOST_TMP0, 0);
		};

		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			(known_address & 3u) == 0 &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address, HOST_TMP0))
		{
			return emit_store_to_host();
		}

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks dirty_pins;
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP0, 3, true))
		{
			return false;
		}

		unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (unaligned_fallback == static_cast<size_t>(-1))
			return false;

		if (!EmitVtlbNonHandlerHostAddress(
				HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback, &dirty_pins) ||
			!emit_store_to_host())
		{
			return false;
		}

		ScalarStoreColdTail tail{
			unaligned_fallback,
			handler_fallback,
			m_code.Size(),
			pc,
			raw_cycles_through_instruction,
			event_exit,
			reinterpret_cast<const void*>(&memWrite32),
			rt,
			ScalarStoreWidth::Word,
		};
		tail.dirty_pins = dirty_pins;
		CaptureScalarStoreValue(&tail);
		m_scalar_store_cold_tails.push_back(tail);
		return true;
	}

	bool BlockCompiler::EmitSWL(u32 op)
	{
		return EmitPartialWordStore(op, true);
	}

	bool BlockCompiler::EmitSWR(u32 op)
	{
		return EmitPartialWordStore(op, false);
	}

	bool BlockCompiler::EmitSD(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		const unsigned rt = RT(op);

		const auto emit_store_to_host = [&]() -> bool
		{
			const int cached_qreg = FindGprQCache(rt);
			if (cached_qreg >= 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectMemoryStores++;
#endif
				return m_code.EmitVstrDImm(static_cast<unsigned>(cached_qreg) * 2, HOST_TMP0, 0);
			}

			unsigned rt_low;
			unsigned rt_high;
			if (!EmitGpr64ValueReadOperands(rt, HOST_TMP2, HOST_TMP3, &rt_low, &rt_high))
				return false;

			// PCSX2 owner: R5900OpcodeImpl.cpp::SD() via vtlb_memWrite64().
			// Unpinned values stay in r2/r3 so Cortex-A9 can issue one STRD;
			// pinned low/high words skip the loads and store as two scalar words.
			if (rt_low == HOST_TMP2 && rt_high == HOST_TMP3)
				return m_code.EmitStrdImm8(HOST_TMP2, HOST_TMP3, HOST_TMP0, 0);

			return m_code.EmitStrImm12(rt_low, HOST_TMP0, 0) &&
				   m_code.EmitStrImm12(rt_high, HOST_TMP0, static_cast<u16>(sizeof(u32)));
		};

		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			(known_address & 7u) == 0 &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address, HOST_TMP0))
		{
			return emit_store_to_host();
		}

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks dirty_pins;
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP0, 7, true))
		{
			return false;
		}

		unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (unaligned_fallback == static_cast<size_t>(-1))
			return false;

		if (!EmitVtlbNonHandlerHostAddress(
				HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback, &dirty_pins))
		{
			return false;
		}

		if (!emit_store_to_host())
			return false;

		ScalarStoreColdTail tail{
			unaligned_fallback,
			handler_fallback,
			m_code.Size(),
			pc,
			raw_cycles_through_instruction,
			event_exit,
			reinterpret_cast<const void*>(&memWrite64),
			rt,
			ScalarStoreWidth::Dword,
		};
		tail.dirty_pins = dirty_pins;
		CaptureScalarStoreValue(&tail);
		m_scalar_store_cold_tails.push_back(tail);
		return true;
	}

	bool BlockCompiler::EmitSDL(u32 op)
	{
		return EmitPartialDwordStore(op, true);
	}

	bool BlockCompiler::EmitSDR(u32 op)
	{
		return EmitPartialDwordStore(op, false);
	}

	bool BlockCompiler::EmitSQ(u32 op)
	{
		const unsigned rt = RT(op);
		constexpr unsigned NEON_VALUE = 0;

		const auto emit_store_to_host = [&](unsigned host_address = HOST_TMP0,
			bool writeback = false) -> bool
		{
			if (rt == 0)
			{
				if (m_resident_raw_gpr0_qword)
				{
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuResidentRawGpr0QwordStoreSelections++;
					g_qemuResidentRawGpr0QwordHotInstructionsElided +=
						m_resident_raw_gpr0_entry_instructions;
#endif
					return writeback ?
						m_code.EmitVst1Q32AlignedWriteback(NEON_VALUE, host_address) :
						m_code.EmitVst1Q32Aligned(NEON_VALUE, host_address);
				}

				if (!EmitLoadRawGpr0KnownZeroFlag(HOST_TMP1) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, 0, true))
	{
					return false;
	}

				const size_t raw_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
				if (raw_fallback == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitVeorQ(NEON_VALUE, NEON_VALUE, NEON_VALUE) ||
					!m_code.EmitVst1Q32Aligned(NEON_VALUE, host_address))
	{
					return false;
	}

				const size_t zero_done = m_code.EmitBranchPlaceholder();
				if (zero_done == static_cast<size_t>(-1))
					return false;

				const size_t raw_fallback_target = m_code.Size();
				return m_code.PatchBranch(raw_fallback, raw_fallback_target, VitaA32::Condition::EQ) &&
					   EmitLoadCpuRegsQ128(GprOffset(0), NEON_VALUE, HOST_TMP1) &&
					   m_code.EmitVst1Q32Aligned(NEON_VALUE, host_address) &&
					   m_code.PatchBranch(zero_done, m_code.Size());
			}

			const int cached_qreg = FindGprQCache(rt);
			if (cached_qreg >= 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectMemoryStores++;
#endif
				return m_code.EmitVst1Q32Aligned(static_cast<unsigned>(cached_qreg), host_address);
			}

			unsigned value_qreg = NEON_VALUE;
			for (unsigned qreg = 0; qreg < MAX_GPR_QCACHE; qreg++)
			{
				if (!IsGprQCacheQregResident(qreg))
	{
					value_qreg = qreg;
					break;
	}
			}

			return EmitLoadGprQ128(rt, value_qreg, HOST_TMP1) &&
				   m_code.EmitVst1Q32Aligned(value_qreg, host_address);
		};

		if (m_resident_vtlb_qword_pointer && op == m_resident_vtlb_qword_store_op)
		{
			if (!emit_store_to_host(HOST_TMP3, true))
				return false;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuResidentVtlbQwordPointerStores++;
			g_qemuResidentVtlbQwordPointerPostIncrementStores++;
			if (m_resident_vtlb_qword_translation_instructions >=
				m_resident_vtlb_qword_guard_instructions)
			{
				g_qemuResidentVtlbQwordPointerHotInstructionsElided +=
					m_resident_vtlb_qword_translation_instructions -
					m_resident_vtlb_qword_guard_instructions;
			}
#endif
			m_qword_store_cold_tails.push_back({
				m_resident_vtlb_qword_handler_fallback,
				m_code.Size(),
				rt,
				m_resident_vtlb_qword_dirty_pins,
			});
			return true;
		}

		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address & ~0x0fu, HOST_TMP0,
				KnownVtlbFastPathKind::Qword))
		{
			return emit_store_to_host();
		}

		size_t handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks dirty_pins;
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!EmitAlignQwordAddress(HOST_TMP0, HOST_TMP1) ||
			!EmitVtlbNonHandlerHostAddress128(
				HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback, &dirty_pins))
		{
			return false;
		}

		if (!emit_store_to_host())
			return false;

		m_qword_store_cold_tails.push_back({
			handler_fallback,
			m_code.Size(),
			rt,
			dirty_pins,
		});
		return true;
	}

	bool BlockCompiler::EmitSWC1(u32 op)
	{
		const unsigned rt = RT(op);

		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			(known_address & 3u) == 0 &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address, HOST_TMP0,
				KnownVtlbFastPathKind::Cop1))
		{
			return m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(rt))) &&
				   m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0);
		}

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks dirty_pins;
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP0, 3, true))
		{
			return false;
		}

		unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (unaligned_fallback == static_cast<size_t>(-1))
			return false;

		if (!EmitVtlbNonHandlerHostAddress(
				HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback, &dirty_pins) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(rt))) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0))
		{
			return false;
		}

		m_cop1_word_memory_cold_tails.push_back({
			unaligned_fallback,
			handler_fallback,
			m_code.Size(),
			rt,
			true,
			dirty_pins,
		});
		return true;
	}

	bool BlockCompiler::EmitSQC2(u32 op)
	{
		const unsigned rt = RT(op);
		constexpr unsigned NEON_VALUE = 0;
		const auto emit_store_to_host = [&]() -> bool {
			if (rt == 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuCop2QwordZeroStoreFastPaths++;
#endif
				// Keep SQC2's memory side effect, but avoid a VU0 state load
				// for VF0's architectural constant.
				return EmitVu0Vf0ConstantQ(NEON_VALUE, HOST_TMP1) &&
					   m_code.EmitVst1Q32(NEON_VALUE, HOST_TMP0);
			}

			return EmitVu0VfAddress(HOST_TMP1, rt) &&
				   m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP1) &&
				   m_code.EmitVst1Q32(NEON_VALUE, HOST_TMP0);
		};

		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address, HOST_TMP0,
				KnownVtlbFastPathKind::Cop2))
		{
			return EmitVu0SyncIfRunning(HOST_TMP0, HOST_TMP5) &&
				   emit_store_to_host();
		}

		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!EmitVtlbNonHandlerHostAddress128(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!EmitVu0SyncIfRunning(HOST_TMP0, HOST_TMP5) ||
			!emit_store_to_host())
		{
			return false;
		}

		m_cop2_qword_memory_cold_tails.push_back({
			handler_fallback,
			m_code.Size(),
			rt,
			true,
		});
		return true;
	}

	bool BlockCompiler::EmitDSLLV(u32 op)
	{
		return EmitShift64LeftVariable(op);
	}

	bool BlockCompiler::EmitDSRLV(u32 op)
	{
		return EmitShift64RightVariable(op, false);
	}

	bool BlockCompiler::EmitDSRAV(u32 op)
	{
		return EmitShift64RightVariable(op, true);
	}

	bool BlockCompiler::EmitDSLL(u32 op)
	{
		return EmitShift64LeftImmediate(op, SA(op));
	}

	bool BlockCompiler::EmitDSRL(u32 op)
	{
		return EmitShift64RightImmediate(op, SA(op), false);
	}

	bool BlockCompiler::EmitDSRA(u32 op)
	{
		return EmitShift64RightImmediate(op, SA(op), true);
	}

	bool BlockCompiler::EmitDSLL32(u32 op)
	{
		return EmitShift64LeftImmediate(op, SA(op) + 32);
	}

	bool BlockCompiler::EmitDSRL32(u32 op)
	{
		return EmitShift64RightImmediate(op, SA(op) + 32, false);
	}

	bool BlockCompiler::EmitDSRA32(u32 op)
	{
		return EmitShift64RightImmediate(op, SA(op) + 32, true);
	}

	bool BlockCompiler::EmitADDU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		const auto emit_store_sign32_dynamic = [&](unsigned guest_reg) {
			unsigned guest_host;
			return EmitGprLowOperand(guest_reg, HOST_TMP0, &guest_host) &&
				   EmitStoreGprSignExtended32FromLow(rd, guest_host);
		};

		const auto emit_add_constant = [&](unsigned guest_reg, u32 constant) {
			if (constant == 0)
				return emit_store_sign32_dynamic(guest_reg);

			unsigned guest_host;
			if (!EmitGprLowOperand(guest_reg, HOST_TMP0, &guest_host))
				return false;

			const unsigned result_reg = SelectGprLowResultHost(rd, HOST_TMP0);
			if (!(m_code.EmitAddImm32(result_reg, guest_host, constant) ||
				  (m_code.EmitMovImm32(HOST_TMP2, constant) &&
				   m_code.EmitAddReg(result_reg, guest_host, HOST_TMP2))))
			{
				return false;
			}

			return EmitStoreGprSignExtended32FromLow(rd, result_reg);
		};

		if (rs == 0 || rt == 0)
		{
			const unsigned src = (rs == 0) ? rt : rs;
			if (src == 0)
				return EmitStoreGprZero64(rd);

			u32 src_value = 0;
			if (FindGprPinHost(src) < 0 && TryGetKnownGprLow(src, &src_value))
			{
				// PCSX2 x86/ix86-32/iR5900Arit.cpp::recADD_const() keeps
				// zero-plus-constant identities in the constant register file.
				return EmitStoreKnownSignExtended32(rd, src_value);
			}

			return emit_store_sign32_dynamic(src);
		}

		u32 rs_value = 0;
		u32 rt_value = 0;
		const bool rs_known = TryGetKnownGprLow(rs, &rs_value);
		const bool rt_known = TryGetKnownGprLow(rt, &rt_value);
		const bool rs_pinned = FindGprPinHost(rs) >= 0;
		const bool rt_pinned = FindGprPinHost(rt) >= 0;
		if (!rs_pinned && !rt_pinned && rs_known && rt_known)
		{
			// PCSX2 x86/ix86-32/iR5900Arit.cpp::recADD_const() keeps this in
			// the constant register file; materialize only the folded result
			// when doing so removes guest-state loads.
			return EmitStoreKnownSignExtended32(rd, rs_value + rt_value);
		}
		if (!rs_pinned && rs_known)
		{
			// PCSX2 iR5900Arit.cpp::recADD_consts() adds a constant RS low word
			// to the runtime RT low word before sign-extending the 32-bit result.
			return emit_add_constant(rt, rs_value);
		}
		if (!rt_pinned && rt_known)
		{
			// PCSX2 iR5900Arit.cpp::recADD_constt() adds a constant RT low word
			// to the runtime RS low word before sign-extending the 32-bit result.
			return emit_add_constant(rs, rt_value);
		}

		unsigned rs_host;
		unsigned rt_host;
		if (!EmitGprLowOperand(rs, HOST_TMP0, &rs_host) ||
			!EmitGprLowOperand(rt, HOST_TMP1, &rt_host))
		{
			return false;
		}

		const unsigned result_reg = SelectGprLowResultHost(rd, HOST_TMP0);
		return m_code.EmitAddReg(result_reg, rs_host, rt_host) &&
			   EmitStoreGprSignExtended32FromLow(rd, result_reg);
	}

	bool BlockCompiler::EmitSUBU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		if (rs == rt)
			return EmitStoreGprZero64(rd);

		const auto emit_store_sign32_dynamic = [&](unsigned guest_reg) {
			unsigned guest_host;
			return EmitGprLowOperand(guest_reg, HOST_TMP0, &guest_host) &&
				   EmitStoreGprSignExtended32FromLow(rd, guest_host);
		};

		const auto emit_subtract_constant = [&](unsigned guest_reg, u32 constant) {
			if (constant == 0)
				return emit_store_sign32_dynamic(guest_reg);

			unsigned guest_host;
			if (!EmitGprLowOperand(guest_reg, HOST_TMP0, &guest_host))
				return false;

			const unsigned result_reg = SelectGprLowResultHost(rd, HOST_TMP0);
			if (!(m_code.EmitSubImm32(result_reg, guest_host, constant) ||
				  (m_code.EmitMovImm32(HOST_TMP2, constant) &&
				   m_code.EmitSubReg(result_reg, guest_host, HOST_TMP2))))
			{
				return false;
			}

			return EmitStoreGprSignExtended32FromLow(rd, result_reg);
		};

		const auto emit_constant_minus_dynamic = [&](unsigned guest_reg, u32 constant) {
			unsigned guest_host;
			if (!EmitGprLowOperand(guest_reg, HOST_TMP2, &guest_host))
				return false;

			const unsigned result_reg = SelectGprLowResultHost(rd, HOST_TMP0);
			bool result_done = m_code.EmitRsbImm32(result_reg, guest_host, constant);
			if (!result_done)
			{
				const unsigned const_reg = (result_reg == guest_host) ? HOST_TMP0 : result_reg;
				result_done = m_code.EmitMovImm32(const_reg, constant) &&
							  m_code.EmitSubReg(result_reg, const_reg, guest_host);
			}

			return result_done && EmitStoreGprSignExtended32FromLow(rd, result_reg);
		};

		if (rt == 0)
		{
			u32 rs_value = 0;
			if (FindGprPinHost(rs) < 0 && TryGetKnownGprLow(rs, &rs_value))
				return EmitStoreKnownSignExtended32(rd, rs_value);

			return emit_store_sign32_dynamic(rs);
		}

		if (rs == 0)
		{
			u32 rt_value = 0;
			if (FindGprPinHost(rt) < 0 && TryGetKnownGprLow(rt, &rt_value))
				return EmitStoreKnownSignExtended32(rd, 0 - rt_value);

			return emit_constant_minus_dynamic(rt, 0);
		}

		u32 rs_value = 0;
		u32 rt_value = 0;
		const bool rs_known = TryGetKnownGprLow(rs, &rs_value);
		const bool rt_known = TryGetKnownGprLow(rt, &rt_value);
		const bool rs_pinned = FindGprPinHost(rs) >= 0;
		const bool rt_pinned = FindGprPinHost(rt) >= 0;
		if (!rs_pinned && !rt_pinned && rs_known && rt_known)
			return EmitStoreKnownSignExtended32(rd, rs_value - rt_value);
		if (!rt_pinned && rt_known)
		{
			// PCSX2 iR5900Arit.cpp::recSUB_constt() subtracts a constant RT low
			// word from runtime RS, then sign-extends the 32-bit result.
			return emit_subtract_constant(rs, rt_value);
		}
		if (!rs_pinned && rs_known)
		{
			// PCSX2 iR5900Arit.cpp::recSUB_consts() materializes constant RS
			// before subtracting runtime RT, then sign-extends the 32-bit result.
			return emit_constant_minus_dynamic(rt, rs_value);
		}

		unsigned rs_host;
		unsigned rt_host;
		if (!EmitGprLowOperand(rs, HOST_TMP0, &rs_host) ||
			!EmitGprLowOperand(rt, HOST_TMP1, &rt_host))
		{
			return false;
		}

		const unsigned result_reg = SelectGprLowResultHost(rd, HOST_TMP0);
		return m_code.EmitSubReg(result_reg, rs_host, rt_host) &&
			   EmitStoreGprSignExtended32FromLow(rd, result_reg);
	}

	bool BlockCompiler::EmitDADDU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		const auto emit_store_dynamic = [&](unsigned guest_reg) {
			if (rd == guest_reg)
				return true;

			bool qcache_store = false;
			if (!TryEmitStoreGprLow64FromQCache(rd, guest_reg, &qcache_store))
				return false;
			if (qcache_store)
				return true;

			unsigned guest_low;
			unsigned guest_high;
			return EmitGpr64ReadOperands(guest_reg, HOST_TMP0, HOST_TMP1, &guest_low, &guest_high) &&
				   EmitStoreGpr64(rd, guest_low, guest_high);
		};

		const auto emit_add_constant = [&](unsigned guest_reg, u32 constant_low, u32 constant_high) {
			if (constant_low == 0 && constant_high == 0)
				return emit_store_dynamic(guest_reg);

			unsigned guest_low;
			unsigned guest_high;
			if (!EmitGpr64ReadOperands(guest_reg, HOST_TMP0, HOST_TMP1, &guest_low, &guest_high))
				return false;

			const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
			const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
			bool low_done = m_code.EmitAddImm32(low_result, guest_low, constant_low, true);
#if defined(VITASX2_QEMU_VALIDATION)
			if (low_done && !CanEncodeA32ModifiedImmediate(constant_low) &&
				CanEncodeA32ModifiedImmediate(0u - constant_low))
			{
				g_qemuInverseFlagImmediateFastPaths++;
			}
#endif
			if (!low_done)
			{
				low_done = m_code.EmitMovImm32(HOST_TMP2, constant_low) &&
					m_code.EmitAddReg(low_result, guest_low, HOST_TMP2, true);
			}
			if (!low_done)
				return false;

			bool high_done = m_code.EmitAdcImm32(high_result, guest_high, constant_high);
#if defined(VITASX2_QEMU_VALIDATION)
			if (high_done && constant_high == 0xffffffffu)
				g_qemuNegativeHighCarryFastPaths++;
			if (high_done && !CanEncodeA32ModifiedImmediate(constant_high) &&
				CanEncodeA32ModifiedImmediate(~constant_high))
			{
				g_qemuInverseCarryImmediateFastPaths++;
			}
			if (high_done && constant_high > 0xffu &&
				CanEncodeA32ModifiedImmediate(constant_high))
			{
				g_qemuCarryModifiedImmediateFastPaths++;
			}
#endif
			if (!high_done)
			{
				high_done = m_code.EmitMovImm32(HOST_TMP2, constant_high) &&
					m_code.EmitAdcReg(high_result, guest_high, HOST_TMP2);
			}
			return high_done && EmitStoreGpr64(rd, low_result, high_result);
		};

		if (rs == 0 || rt == 0)
		{
			const unsigned src = (rs == 0) ? rt : rs;
			if (src == 0)
				return EmitStoreGprZero64(rd);

			u32 src_low_value = 0;
			u32 src_high_value = 0;
			if (FindGprPinHost(src) < 0 && TryGetKnownGpr64(src, &src_low_value, &src_high_value))
			{
				// PCSX2 x86/ix86-32/iR5900Arit.cpp::recDADD_const() keeps
				// zero-plus-constant identities as exact low64 constants.
				return EmitStoreKnown64(rd, src_low_value, src_high_value);
			}

			return emit_store_dynamic(src);
		}

		u32 rs_low_value = 0;
		u32 rs_high_value = 0;
		u32 rt_low_value = 0;
		u32 rt_high_value = 0;
		const bool rs_known = TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value);
		const bool rt_known = TryGetKnownGpr64(rt, &rt_low_value, &rt_high_value);
		const bool rs_pinned = FindGprPinHost(rs) >= 0;
		const bool rt_pinned = FindGprPinHost(rt) >= 0;
		if (!rs_pinned && !rt_pinned && rs_known && rt_known)
		{
			// PCSX2 x86/ix86-32/iR5900Arit.cpp::recDADD_const() folds the
			// full low64 sum in the constant register file.
			const u64 lhs = (static_cast<u64>(rs_high_value) << 32) | rs_low_value;
			const u64 rhs = (static_cast<u64>(rt_high_value) << 32) | rt_low_value;
			const u64 result = lhs + rhs;
			return EmitStoreKnown64(rd, static_cast<u32>(result), static_cast<u32>(result >> 32));
		}
		if (!rs_pinned && rs_known)
		{
			// PCSX2 iR5900Arit.cpp::recDADD_consts() adds a full-width constant
			// source to the runtime RT value.
			return emit_add_constant(rt, rs_low_value, rs_high_value);
		}
		if (!rt_pinned && rt_known)
		{
			// PCSX2 iR5900Arit.cpp::recDADD_constt() adds a full-width constant
			// target operand to the runtime RS value.
			return emit_add_constant(rs, rt_low_value, rt_high_value);
		}

		unsigned rs_low;
		unsigned rt_low;
		unsigned rs_high;
		unsigned rt_high;
		if (!EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high) ||
			!EmitGpr64ReadOperands(rt, HOST_TMP2, HOST_TMP3, &rt_low, &rt_high))
		{
			return false;
		}

		const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
		const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
		return m_code.EmitAddReg(low_result, rs_low, rt_low, true) &&
			   m_code.EmitAdcReg(high_result, rs_high, rt_high) &&
			   EmitStoreGpr64(rd, low_result, high_result);
	}

	bool BlockCompiler::EmitDSUBU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		if (rs == rt)
			return EmitStoreGprZero64(rd);

		const auto emit_store_dynamic = [&](unsigned guest_reg) {
			if (rd == guest_reg)
				return true;

			bool qcache_store = false;
			if (!TryEmitStoreGprLow64FromQCache(rd, guest_reg, &qcache_store))
				return false;
			if (qcache_store)
				return true;

			unsigned guest_low;
			unsigned guest_high;
			return EmitGpr64ReadOperands(guest_reg, HOST_TMP0, HOST_TMP1, &guest_low, &guest_high) &&
				   EmitStoreGpr64(rd, guest_low, guest_high);
		};

		const auto emit_subtract_constant = [&](unsigned guest_reg, u32 constant_low, u32 constant_high) {
			if (constant_low == 0 && constant_high == 0)
				return emit_store_dynamic(guest_reg);

			unsigned guest_low;
			unsigned guest_high;
			if (!EmitGpr64ReadOperands(guest_reg, HOST_TMP0, HOST_TMP1, &guest_low, &guest_high))
				return false;

			const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
			const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
			bool low_done = m_code.EmitSubImm32(low_result, guest_low, constant_low, true);
#if defined(VITASX2_QEMU_VALIDATION)
			if (low_done && !CanEncodeA32ModifiedImmediate(constant_low) &&
				CanEncodeA32ModifiedImmediate(0u - constant_low))
			{
				g_qemuInverseFlagImmediateFastPaths++;
			}
#endif
			if (!low_done)
			{
				low_done = m_code.EmitMovImm32(HOST_TMP2, constant_low) &&
					m_code.EmitSubReg(low_result, guest_low, HOST_TMP2, true);
			}
			if (!low_done)
				return false;

			bool high_done = m_code.EmitSbcImm32(high_result, guest_high, constant_high);
#if defined(VITASX2_QEMU_VALIDATION)
			if (high_done && constant_high == 0xffffffffu)
				g_qemuNegativeHighCarryFastPaths++;
			if (high_done && !CanEncodeA32ModifiedImmediate(constant_high) &&
				CanEncodeA32ModifiedImmediate(~constant_high))
			{
				g_qemuInverseCarryImmediateFastPaths++;
			}
			if (high_done && constant_high > 0xffu &&
				CanEncodeA32ModifiedImmediate(constant_high))
			{
				g_qemuCarryModifiedImmediateFastPaths++;
			}
#endif
			if (!high_done)
			{
				high_done = m_code.EmitMovImm32(HOST_TMP2, constant_high) &&
					m_code.EmitSbcReg(high_result, guest_high, HOST_TMP2);
			}
			return high_done && EmitStoreGpr64(rd, low_result, high_result);
		};

		const auto emit_constant_minus_dynamic = [&](unsigned guest_reg, u32 constant_low, u32 constant_high) {
			unsigned guest_low;
			unsigned guest_high;
			if (!EmitGpr64ReadOperands(guest_reg, HOST_TMP2, HOST_TMP3, &guest_low, &guest_high))
				return false;

			const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
			const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
			bool low_done = m_code.EmitRsbImm32(low_result, guest_low, constant_low, true);
			if (!low_done)
			{
				const unsigned const_low = (low_result == guest_low) ? HOST_TMP0 : low_result;
				low_done = m_code.EmitMovImm32(const_low, constant_low) &&
						   m_code.EmitSubReg(low_result, const_low, guest_low, true);
			}

			if (!low_done)
				return false;

			bool high_done = m_code.EmitRscImm32(high_result, guest_high, constant_high);
#if defined(VITASX2_QEMU_VALIDATION)
			if (high_done)
				g_qemuReverseSubtractCarryImmediateFastPaths++;
#endif
			if (!high_done)
			{
				high_done = m_code.EmitMovImm32(HOST_TMP1, constant_high) &&
					m_code.EmitSbcReg(high_result, HOST_TMP1, guest_high);
			}

			return high_done && EmitStoreGpr64(rd, low_result, high_result);
		};

		if (rt == 0)
		{
			u32 rs_low_value = 0;
			u32 rs_high_value = 0;
			if (FindGprPinHost(rs) < 0 && TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value))
				return EmitStoreKnown64(rd, rs_low_value, rs_high_value);

			return emit_store_dynamic(rs);
		}

		if (rs == 0)
		{
			u32 rt_low_value = 0;
			u32 rt_high_value = 0;
			if (FindGprPinHost(rt) < 0 && TryGetKnownGpr64(rt, &rt_low_value, &rt_high_value))
			{
				const u64 rhs = (static_cast<u64>(rt_high_value) << 32) | rt_low_value;
				const u64 result = 0 - rhs;
				return EmitStoreKnown64(rd, static_cast<u32>(result), static_cast<u32>(result >> 32));
			}

			unsigned rt_low;
			unsigned rt_high;
			if (!EmitGpr64ReadOperands(rt, HOST_TMP2, HOST_TMP3, &rt_low, &rt_high))
				return false;

			const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
			const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
			if (!m_code.EmitRsbImm32(low_result, rt_low, 0, true) ||
				!m_code.EmitRscImm32(high_result, rt_high, 0))
			{
				return false;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuReverseSubtractCarryImmediateFastPaths++;
#endif
			return EmitStoreGpr64(rd, low_result, high_result);
		}

		u32 rs_low_value = 0;
		u32 rs_high_value = 0;
		u32 rt_low_value = 0;
		u32 rt_high_value = 0;
		const bool rs_known = TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value);
		const bool rt_known = TryGetKnownGpr64(rt, &rt_low_value, &rt_high_value);
		const bool rs_pinned = FindGprPinHost(rs) >= 0;
		const bool rt_pinned = FindGprPinHost(rt) >= 0;
		if (!rs_pinned && !rt_pinned && rs_known && rt_known)
		{
			// PCSX2 x86/ix86-32/iR5900Arit.cpp::recDSUB_const() folds the
			// full low64 difference in the constant register file.
			const u64 lhs = (static_cast<u64>(rs_high_value) << 32) | rs_low_value;
			const u64 rhs = (static_cast<u64>(rt_high_value) << 32) | rt_low_value;
			const u64 result = lhs - rhs;
			return EmitStoreKnown64(rd, static_cast<u32>(result), static_cast<u32>(result >> 32));
		}
		if (!rt_pinned && rt_known)
		{
			// PCSX2 iR5900Arit.cpp::recDSUB_constt() subtracts a full-width
			// constant RT from the runtime RS value.
			return emit_subtract_constant(rs, rt_low_value, rt_high_value);
		}
		if (!rs_pinned && rs_known)
		{
			// PCSX2 iR5900Arit.cpp::recDSUB_consts() materializes the full-width
			// constant RS before subtracting the runtime RT value.
			return emit_constant_minus_dynamic(rt, rs_low_value, rs_high_value);
		}

		unsigned rs_low;
		unsigned rt_low;
		unsigned rs_high;
		unsigned rt_high;
		if (!EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high) ||
			!EmitGpr64ReadOperands(rt, HOST_TMP2, HOST_TMP3, &rt_low, &rt_high))
		{
			return false;
		}

		const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
		const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
		return m_code.EmitSubReg(low_result, rs_low, rt_low, true) &&
			   m_code.EmitSbcReg(high_result, rs_high, rt_high) &&
			   EmitStoreGpr64(rd, low_result, high_result);
	}

	bool BlockCompiler::EmitAND(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::AND() assigns only UD[0].
		// Fold $zero/self identities before loading both low-64 operands.
		if (rs == 0 || rt == 0)
			return EmitStoreGprZero64(rd);
		if (rs == rt)
		{
			if (rd == rs)
				return true;

			bool qcache_store = false;
			if (!TryEmitStoreGprLow64FromQCache(rd, rs, &qcache_store))
				return false;
			if (qcache_store)
				return true;

			unsigned rs_low;
			unsigned rs_high;
			return EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high) &&
				   EmitStoreGpr64(rd, rs_low, rs_high);
		}

		u32 rs_low_value = 0;
		u32 rs_high_value = 0;
		u32 rt_low_value = 0;
		u32 rt_high_value = 0;
		const bool rs_pinned = FindGprPinHost(rs) >= 0;
		const bool rt_pinned = FindGprPinHost(rt) >= 0;
		const bool rs_known = TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value);
		const bool rt_known = TryGetKnownGpr64(rt, &rt_low_value, &rt_high_value);
		if (!rs_pinned && !rt_pinned && rs_known && rt_known)
		{
			// PCSX2 x86/ix86-32/iR5900Arit.cpp::recAND_const() folds the full
			// low-64 register constants. Only use it when it replaces two real
			// guest-state loads rather than pinned register ALU.
			return EmitStoreKnown64(rd, rs_low_value & rt_low_value, rs_high_value & rt_high_value);
		}
		const auto emit_copy_source = [&](unsigned src) {
			if (rd == src)
				return true;

			bool qcache_store = false;
			if (!TryEmitStoreGprLow64FromQCache(rd, src, &qcache_store))
				return false;
			if (qcache_store)
				return true;

			unsigned src_low;
			unsigned src_high;
			return EmitGpr64ReadOperands(src, HOST_TMP0, HOST_TMP1, &src_low, &src_high) &&
				   EmitStoreGpr64(rd, src_low, src_high);
		};
		const auto emit_and_constant = [&](unsigned src, u32 constant_low, u32 constant_high) {
			if (constant_low == 0 && constant_high == 0)
				return EmitStoreGprZero64(rd);
			if (constant_low == 0xffffffffu && constant_high == 0xffffffffu)
				return emit_copy_source(src);

			unsigned src_low;
			unsigned src_high;
			if (!EmitGpr64ReadOperands(src, HOST_TMP0, HOST_TMP1, &src_low, &src_high))
				return false;

			const bool low_ok = constant_low == 0 ?
				m_code.EmitMovImm8(HOST_TMP0, 0) :
				(constant_low == 0xffffffffu ?
					 (src_low == HOST_TMP0 ||
						 m_code.EmitMovRegShiftImm(HOST_TMP0, src_low, VitaA32::ShiftType::LSL, 0)) :
					 EmitAndImm32OrReg(HOST_TMP0, src_low, constant_low, HOST_TMP2));
			if (!low_ok)
				return false;

			unsigned high_result = HOST_TMP1;
			const bool high_ok = constant_high == 0 ?
				m_code.EmitMovImm8(HOST_TMP1, 0) :
				(constant_high == 0xffffffffu ?
					 ((high_result = src_high), true) :
					 EmitAndImm32OrReg(HOST_TMP1, src_high, constant_high, HOST_TMP2));
			return low_ok && high_ok && EmitStoreGpr64(rd, HOST_TMP0, high_result);
		};
		if (!rs_pinned && rs_known && (!rt_pinned || IsCheapA32AndConstant64(rs_low_value, rs_high_value)))
		{
			// PCSX2 iR5900Arit.cpp::recAND_consts() applies a constant RS mask
			// to the runtime RT value.
			return emit_and_constant(rt, rs_low_value, rs_high_value);
		}
		if (!rt_pinned && rt_known && (!rs_pinned || IsCheapA32AndConstant64(rt_low_value, rt_high_value)))
		{
			// PCSX2 iR5900Arit.cpp::recAND_constt() applies a constant RT mask
			// to the runtime RS value.
			return emit_and_constant(rs, rt_low_value, rt_high_value);
		}

		unsigned rs_low;
		unsigned rt_low;
		unsigned rs_high;
		unsigned rt_high;
		if (!EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high) ||
			!EmitGpr64ReadOperands(rt, HOST_TMP2, HOST_TMP3, &rt_low, &rt_high))
		{
			return false;
		}

		const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
		const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
		return m_code.EmitAndReg(low_result, rs_low, rt_low) &&
			   m_code.EmitAndReg(high_result, rs_high, rt_high) &&
			   EmitStoreGpr64(rd, low_result, high_result);
	}

	bool BlockCompiler::EmitOR(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::OR() assigns only UD[0].
		if (rs == 0 || rt == 0 || rs == rt)
		{
			const unsigned src = (rs == 0) ? rt : rs;
			if (src == 0)
				return EmitStoreGprZero64(rd);
			if (rd == src)
				return true;

			bool qcache_store = false;
			if (!TryEmitStoreGprLow64FromQCache(rd, src, &qcache_store))
				return false;
			if (qcache_store)
				return true;

			unsigned src_low;
			unsigned src_high;
			return EmitGpr64ReadOperands(src, HOST_TMP0, HOST_TMP1, &src_low, &src_high) &&
				   EmitStoreGpr64(rd, src_low, src_high);
		}

		u32 rs_low_value = 0;
		u32 rs_high_value = 0;
		u32 rt_low_value = 0;
		u32 rt_high_value = 0;
		const bool rs_pinned = FindGprPinHost(rs) >= 0;
		const bool rt_pinned = FindGprPinHost(rt) >= 0;
		const bool rs_known = TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value);
		const bool rt_known = TryGetKnownGpr64(rt, &rt_low_value, &rt_high_value);
		if (!rs_pinned && !rt_pinned && rs_known && rt_known)
		{
			// PCSX2 x86/ix86-32/iR5900Arit.cpp::recOR_const() folds the full
			// low-64 register constants.
			return EmitStoreKnown64(rd, rs_low_value | rt_low_value, rs_high_value | rt_high_value);
		}
		const auto emit_copy_source = [&](unsigned src) {
			if (rd == src)
				return true;

			bool qcache_store = false;
			if (!TryEmitStoreGprLow64FromQCache(rd, src, &qcache_store))
				return false;
			if (qcache_store)
				return true;

			unsigned src_low;
			unsigned src_high;
			return EmitGpr64ReadOperands(src, HOST_TMP0, HOST_TMP1, &src_low, &src_high) &&
				   EmitStoreGpr64(rd, src_low, src_high);
		};
		const auto emit_or_constant = [&](unsigned src, u32 constant_low, u32 constant_high) {
			if (constant_low == 0 && constant_high == 0)
				return emit_copy_source(src);
			if (constant_low == 0xffffffffu && constant_high == 0xffffffffu)
				return EmitStoreKnown64(rd, 0xffffffffu, 0xffffffffu);

			unsigned src_low;
			unsigned src_high;
			if (!EmitGpr64ReadOperands(src, HOST_TMP0, HOST_TMP1, &src_low, &src_high))
				return false;

			const bool low_ok = constant_low == 0 ?
				(src_low == HOST_TMP0 ||
					m_code.EmitMovRegShiftImm(HOST_TMP0, src_low, VitaA32::ShiftType::LSL, 0)) :
				(constant_low == 0xffffffffu ?
					 m_code.EmitMovImm32(HOST_TMP0, 0xffffffffu) :
					 EmitOrrImm32OrReg(HOST_TMP0, src_low, constant_low, HOST_TMP2));
			if (!low_ok)
				return false;

			unsigned high_result = HOST_TMP1;
			const bool high_ok = constant_high == 0 ?
				((high_result = src_high), true) :
				(constant_high == 0xffffffffu ?
					 m_code.EmitMovImm32(HOST_TMP1, 0xffffffffu) :
					 EmitOrrImm32OrReg(HOST_TMP1, src_high, constant_high, HOST_TMP2));
			return low_ok && high_ok && EmitStoreGpr64(rd, HOST_TMP0, high_result);
		};
		if (!rs_pinned && rs_known && (!rt_pinned || IsCheapA32LogicalConstant64(rs_low_value, rs_high_value)))
		{
			// PCSX2 iR5900Arit.cpp::recOR_consts() applies a constant RS mask
			// to the runtime RT value.
			return emit_or_constant(rt, rs_low_value, rs_high_value);
		}
		if (!rt_pinned && rt_known && (!rs_pinned || IsCheapA32LogicalConstant64(rt_low_value, rt_high_value)))
		{
			// PCSX2 iR5900Arit.cpp::recOR_constt() applies a constant RT mask
			// to the runtime RS value.
			return emit_or_constant(rs, rt_low_value, rt_high_value);
		}

		unsigned rs_low;
		unsigned rt_low;
		unsigned rs_high;
		unsigned rt_high;
		if (!EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high) ||
			!EmitGpr64ReadOperands(rt, HOST_TMP2, HOST_TMP3, &rt_low, &rt_high))
		{
			return false;
		}

		const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
		const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
		return m_code.EmitOrrReg(low_result, rs_low, rt_low) &&
			   m_code.EmitOrrReg(high_result, rs_high, rt_high) &&
			   EmitStoreGpr64(rd, low_result, high_result);
	}

	bool BlockCompiler::EmitXOR(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::XOR() assigns only UD[0].
		if (rs == rt)
			return EmitStoreGprZero64(rd);
		if (rs == 0 || rt == 0)
		{
			const unsigned src = (rs == 0) ? rt : rs;
			if (rd == src)
				return true;

			bool qcache_store = false;
			if (!TryEmitStoreGprLow64FromQCache(rd, src, &qcache_store))
				return false;
			if (qcache_store)
				return true;

			unsigned src_low;
			unsigned src_high;
			return EmitGpr64ReadOperands(src, HOST_TMP0, HOST_TMP1, &src_low, &src_high) &&
				   EmitStoreGpr64(rd, src_low, src_high);
		}

		u32 rs_low_value = 0;
		u32 rs_high_value = 0;
		u32 rt_low_value = 0;
		u32 rt_high_value = 0;
		const bool rs_pinned = FindGprPinHost(rs) >= 0;
		const bool rt_pinned = FindGprPinHost(rt) >= 0;
		const bool rs_known = TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value);
		const bool rt_known = TryGetKnownGpr64(rt, &rt_low_value, &rt_high_value);
		if (!rs_pinned && !rt_pinned && rs_known && rt_known)
		{
			// PCSX2 x86/ix86-32/iR5900Arit.cpp::recXOR_const() folds the full
			// low-64 register constants.
			return EmitStoreKnown64(rd, rs_low_value ^ rt_low_value, rs_high_value ^ rt_high_value);
		}
		const auto emit_copy_source = [&](unsigned src) {
			if (rd == src)
				return true;

			bool qcache_store = false;
			if (!TryEmitStoreGprLow64FromQCache(rd, src, &qcache_store))
				return false;
			if (qcache_store)
				return true;

			unsigned src_low;
			unsigned src_high;
			return EmitGpr64ReadOperands(src, HOST_TMP0, HOST_TMP1, &src_low, &src_high) &&
				   EmitStoreGpr64(rd, src_low, src_high);
		};
		const auto emit_xor_constant = [&](unsigned src, u32 constant_low, u32 constant_high) {
			if (constant_low == 0 && constant_high == 0)
				return emit_copy_source(src);

			unsigned src_low;
			unsigned src_high;
			if (!EmitGpr64ReadOperands(src, HOST_TMP0, HOST_TMP1, &src_low, &src_high))
				return false;

			const bool low_ok = constant_low == 0 ?
				(src_low == HOST_TMP0 ||
					m_code.EmitMovRegShiftImm(HOST_TMP0, src_low, VitaA32::ShiftType::LSL, 0)) :
				EmitEorImm32OrReg(HOST_TMP0, src_low, constant_low, HOST_TMP2);
			if (!low_ok)
				return false;

			unsigned high_result = HOST_TMP1;
			const bool high_ok = constant_high == 0 ?
				((high_result = src_high), true) :
				EmitEorImm32OrReg(HOST_TMP1, src_high, constant_high, HOST_TMP2);
			return low_ok && high_ok && EmitStoreGpr64(rd, HOST_TMP0, high_result);
		};
		if (!rs_pinned && rs_known && (!rt_pinned || IsCheapA32LogicalConstant64(rs_low_value, rs_high_value)))
		{
			// PCSX2 iR5900Arit.cpp::recXOR_consts() applies a constant RS mask
			// to the runtime RT value.
			return emit_xor_constant(rt, rs_low_value, rs_high_value);
		}
		if (!rt_pinned && rt_known && (!rs_pinned || IsCheapA32LogicalConstant64(rt_low_value, rt_high_value)))
		{
			// PCSX2 iR5900Arit.cpp::recXOR_constt() applies a constant RT mask
			// to the runtime RS value.
			return emit_xor_constant(rs, rt_low_value, rt_high_value);
		}

		unsigned rs_low;
		unsigned rt_low;
		unsigned rs_high;
		unsigned rt_high;
		if (!EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high) ||
			!EmitGpr64ReadOperands(rt, HOST_TMP2, HOST_TMP3, &rt_low, &rt_high))
		{
			return false;
		}

		const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
		const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
		return m_code.EmitEorReg(low_result, rs_low, rt_low) &&
			   m_code.EmitEorReg(high_result, rs_high, rt_high) &&
			   EmitStoreGpr64(rd, low_result, high_result);
	}

	bool BlockCompiler::EmitNOR(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::NOR() assigns only UD[0].
		if (rs == 0 && rt == 0)
		{
			return EmitStoreKnown64(rd, 0xffffffffu, 0xffffffffu);
		}
		if (rs == 0 || rt == 0 || rs == rt)
		{
			const unsigned src = (rs == 0) ? rt : rs;
			bool qcache_store = false;
			if (!TryEmitStoreGprLow64InvertFromQCache(rd, src, &qcache_store))
				return false;
			if (qcache_store)
				return true;

			unsigned src_low;
			unsigned src_high;
			if (!EmitGpr64ReadOperands(src, HOST_TMP0, HOST_TMP1, &src_low, &src_high))
				return false;

			const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
			const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
			return m_code.EmitMvnReg(low_result, src_low) &&
				   m_code.EmitMvnReg(high_result, src_high) &&
				   EmitStoreGpr64(rd, low_result, high_result);
		}

		u32 rs_low_value = 0;
		u32 rs_high_value = 0;
		u32 rt_low_value = 0;
		u32 rt_high_value = 0;
		const bool rs_pinned = FindGprPinHost(rs) >= 0;
		const bool rt_pinned = FindGprPinHost(rt) >= 0;
		const bool rs_known = TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value);
		const bool rt_known = TryGetKnownGpr64(rt, &rt_low_value, &rt_high_value);
		if (!rs_pinned && !rt_pinned && rs_known && rt_known)
		{
			// PCSX2 x86/ix86-32/iR5900Arit.cpp::recNOR_const() folds the full
			// low-64 register constants.
			return EmitStoreKnown64(rd, ~(rs_low_value | rt_low_value), ~(rs_high_value | rt_high_value));
		}
		const auto emit_nor_constant = [&](unsigned src, u32 constant_low, u32 constant_high) {
			if (constant_low == 0xffffffffu && constant_high == 0xffffffffu)
				return EmitStoreGprZero64(rd);

			unsigned src_low;
			unsigned src_high;
			if (!EmitGpr64ReadOperands(src, HOST_TMP0, HOST_TMP1, &src_low, &src_high))
				return false;

			const bool low_ok = constant_low == 0 ?
				m_code.EmitMvnReg(HOST_TMP0, src_low) :
				(constant_low == 0xffffffffu ?
					 m_code.EmitMovImm8(HOST_TMP0, 0) :
					 (EmitOrrImm32OrReg(HOST_TMP0, src_low, constant_low, HOST_TMP2) &&
						 m_code.EmitMvnReg(HOST_TMP0, HOST_TMP0)));
			if (!low_ok)
				return false;

			const bool high_ok = constant_high == 0 ?
				m_code.EmitMvnReg(HOST_TMP1, src_high) :
				(constant_high == 0xffffffffu ?
					 m_code.EmitMovImm8(HOST_TMP1, 0) :
					 (EmitOrrImm32OrReg(HOST_TMP1, src_high, constant_high, HOST_TMP2) &&
						 m_code.EmitMvnReg(HOST_TMP1, HOST_TMP1)));
			return low_ok && high_ok && EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
		};
		if (!rs_pinned && rs_known && (!rt_pinned || IsCheapA32LogicalConstant64(rs_low_value, rs_high_value)))
		{
			// PCSX2 iR5900Arit.cpp::recNOR_consts() applies a constant RS mask
			// before inverting the runtime RT result.
			return emit_nor_constant(rt, rs_low_value, rs_high_value);
		}
		if (!rt_pinned && rt_known && (!rs_pinned || IsCheapA32LogicalConstant64(rt_low_value, rt_high_value)))
		{
			// PCSX2 iR5900Arit.cpp::recNOR_constt() applies a constant RT mask
			// before inverting the runtime RS result.
			return emit_nor_constant(rs, rt_low_value, rt_high_value);
		}

		unsigned rs_low;
		unsigned rt_low;
		unsigned rs_high;
		unsigned rt_high;
		if (!EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high) ||
			!EmitGpr64ReadOperands(rt, HOST_TMP2, HOST_TMP3, &rt_low, &rt_high))
		{
			return false;
		}

		const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
		const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
		return m_code.EmitOrrReg(low_result, rs_low, rt_low) &&
			   m_code.EmitOrrReg(high_result, rs_high, rt_high) &&
			   m_code.EmitMvnReg(low_result, low_result) &&
			   m_code.EmitMvnReg(high_result, high_result) &&
			   EmitStoreGpr64(rd, low_result, high_result);
	}

	bool BlockCompiler::EmitSLT(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLT() compares signed UD[0] and
		// writes the 0/1 result back to UD[0].
		if (rs == rt)
			return EmitStoreGprZero64(rd);

		u32 rs_low_value = 0;
		u32 rs_high_value = 0;
		u32 rt_low_value = 0;
		u32 rt_high_value = 0;
		const bool rs_pinned = FindGprPinHost(rs) >= 0;
		const bool rt_pinned = FindGprPinHost(rt) >= 0;
		const bool rs_known = TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value);
		const bool rt_known = TryGetKnownGpr64(rt, &rt_low_value, &rt_high_value);
		if (rs_known && rt_known)
		{
			// PCSX2 x86/ix86-32/iR5900Arit.cpp::recSLT_const() folds the full
			// signed low64 comparison when both operands are constant.
			const u64 rs_value = (static_cast<u64>(rs_high_value) << 32) | rs_low_value;
			const u64 rt_value = (static_cast<u64>(rt_high_value) << 32) | rt_low_value;
			const bool result = static_cast<s64>(rs_value) < static_cast<s64>(rt_value);
			return EmitStoreKnownZeroExtended32(rd, result ? 1 : 0);
		}
		if (rt == 0)
		{
			unsigned rs_high;
			const unsigned result_reg = SelectGprLowResultHost(rd, HOST_TMP0);
			return EmitGprWordOperand(rs, 1, HOST_TMP0, &rs_high) &&
				   m_code.EmitMovRegShiftImm(result_reg, rs_high, VitaA32::ShiftType::LSR, 31) &&
				   EmitStoreGprZeroExtended32FromLow(rd, result_reg);
		}
		if (!rs_pinned && rs_known && IsCheapA32CompareConstant64(rs_low_value, rs_high_value))
		{
			// PCSX2 iR5900Arit.cpp::recSLT_consts() compares a constant RS
			// against the runtime RT value.
			return EmitSetLessThan64Known(rd, true, rt, rs_low_value, rs_high_value, true);
		}
		if (!rt_pinned && rt_known && IsCheapA32CompareConstant64(rt_low_value, rt_high_value))
		{
			// PCSX2 iR5900Arit.cpp::recSLT_constt() compares the runtime RS
			// value against a constant RT.
			return EmitSetLessThan64Known(rd, true, rs, rt_low_value, rt_high_value, false);
		}

		unsigned rs_low;
		unsigned rt_low;
		unsigned rs_high;
		unsigned rt_high;
		return EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high) &&
			   EmitGpr64ReadOperands(rt, HOST_TMP2, HOST_TMP3, &rt_low, &rt_high) &&
			   EmitSetLessThan64(rd, true, rs_low, rs_high, rt_low, rt_high);
	}

	bool BlockCompiler::EmitSLTU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLTU() compares unsigned UD[0].
		if (rs == rt || rt == 0)
			return EmitStoreGprZero64(rd);

		u32 rs_low_value = 0;
		u32 rs_high_value = 0;
		u32 rt_low_value = 0;
		u32 rt_high_value = 0;
		const bool rs_pinned = FindGprPinHost(rs) >= 0;
		const bool rt_pinned = FindGprPinHost(rt) >= 0;
		const bool rs_known = TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value);
		const bool rt_known = TryGetKnownGpr64(rt, &rt_low_value, &rt_high_value);
		if (rs_known && rt_known)
		{
			// PCSX2 x86/ix86-32/iR5900Arit.cpp::recSLTU_const() folds the full
			// unsigned low64 comparison when both operands are constant.
			const u64 rs_value = (static_cast<u64>(rs_high_value) << 32) | rs_low_value;
			const u64 rt_value = (static_cast<u64>(rt_high_value) << 32) | rt_low_value;
			return EmitStoreKnownZeroExtended32(rd, rs_value < rt_value ? 1 : 0);
		}
		if (!rs_pinned && rs_known && IsCheapA32CompareConstant64(rs_low_value, rs_high_value))
		{
			// PCSX2 iR5900Arit.cpp::recSLTU_consts() compares a constant RS
			// against the runtime RT value.
			return EmitSetLessThan64Known(rd, false, rt, rs_low_value, rs_high_value, true);
		}
		if (!rt_pinned && rt_known && IsCheapA32CompareConstant64(rt_low_value, rt_high_value))
		{
			// PCSX2 iR5900Arit.cpp::recSLTU_constt() compares the runtime RS
			// value against a constant RT.
			return EmitSetLessThan64Known(rd, false, rs, rt_low_value, rt_high_value, false);
		}

		unsigned rs_low;
		unsigned rt_low;
		unsigned rs_high;
		unsigned rt_high;
		return EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high) &&
			   EmitGpr64ReadOperands(rt, HOST_TMP2, HOST_TMP3, &rt_low, &rt_high) &&
			   EmitSetLessThan64(rd, false, rs_low, rs_high, rt_low, rt_high);
	}

	bool BlockCompiler::EmitShift32Immediate(u32 op, VitaA32::ShiftType shift, unsigned amount)
	{
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		const unsigned sa = amount & 0x1f;

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLL()/SRL()/SRA(). Shifting
		// register zero still writes signed-extended zero.
		if (rt == 0)
			return EmitStoreGprZero64(rd);

		u32 rt_value = 0;
		if (FindGprPinHost(rt) < 0 && TryGetKnownGprLow(rt, &rt_value))
		{
			u32 result = rt_value;
			if (sa != 0)
			{
				switch (shift)
	{
					case VitaA32::ShiftType::LSL:
						result = rt_value << sa;
						break;
					case VitaA32::ShiftType::LSR:
						result = rt_value >> sa;
						break;
					case VitaA32::ShiftType::ASR:
						result = static_cast<u32>(static_cast<s32>(rt_value) >> sa);
						break;
					default:
						break;
	}
			}

			return EmitStoreKnownSignExtended32(rd, result);
		}

		unsigned rt_host;
		if (!EmitGprLowOperand(rt, HOST_TMP0, &rt_host))
			return false;

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLL()/SRL()/SRA(). ARM immediate
		// LSR/ASR with amount 0 encodes a shift of 32; the R5900 sa=0 case
		// leaves the low word unchanged, so only the final sign extension is
		// needed.
		if (sa != 0)
		{
			const unsigned result_reg = SelectGprLowResultHost(rd, HOST_TMP0);
			if (!m_code.EmitMovRegShiftImm(result_reg, rt_host, shift, static_cast<u8>(sa)))
				return false;

			rt_host = result_reg;
		}

		return EmitStoreGprSignExtended32FromLow(rd, rt_host);
	}

	bool BlockCompiler::EmitShift32Variable(u32 op, VitaA32::ShiftType shift)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLLV()/SRLV()/SRAV(). The shift
		// amount is irrelevant when the source register is zero.
		if (rt == 0)
			return EmitStoreGprZero64(rd);

		u32 rs_value = 0;
		if (TryGetKnownGprLow(rs, &rs_value))
		{
			// PCSX2 x86/ix86-32/iR5900Shift.cpp::recSLLV_consts(),
			// recSRLV_consts(), and recSRAV_consts() route a known RS through
			// the immediate-shift lowering while RT remains dynamic.
			const bool emitted = EmitShift32Immediate(op, shift, rs_value);
#if defined(VITASX2_QEMU_VALIDATION)
			if (emitted)
				g_qemuKnownVariableShiftImmediateFastPaths++;
#endif
			return emitted;
		}

		// Register zero supplies a shift amount of 0; the 32-bit result still
		// needs the owner's sign-extension into the low 64-bit lane.
		unsigned rt_host;
		if (!EmitGprLowOperand(rt, HOST_TMP0, &rt_host))
			return false;

		if (rs != 0)
		{
			unsigned rs_host;
			if (!EmitGprLowOperand(rs, HOST_TMP2, &rs_host) ||
				!m_code.EmitAndImm8(HOST_TMP2, rs_host, 0x1f))
			{
				return false;
			}

			const unsigned result_reg = SelectGprLowResultHost(rd, HOST_TMP0);
			if (!m_code.EmitMovRegShiftReg(result_reg, rt_host, shift, HOST_TMP2))
				return false;

			rt_host = result_reg;
		}

		return EmitStoreGprSignExtended32FromLow(rd, rt_host);
	}

	bool BlockCompiler::EmitShift64LeftImmediate(u32 op, unsigned amount)
	{
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::DSLL()/DSLL32(). Shifting
		// register zero writes zero for every immediate amount.
		if (rt == 0)
			return EmitStoreGprZero64(rd);

		u32 rt_low_value = 0;
		u32 rt_high_value = 0;
		if (FindGprPinHost(rt) < 0 && TryGetKnownGpr64(rt, &rt_low_value, &rt_high_value))
		{
			// PCSX2's x86 constant register file folds DSLL/DSLL32 as a full
			// low-64 value; require the exact high word before replacing loads.
			const u64 value = (static_cast<u64>(rt_high_value) << 32) | rt_low_value;
			const u64 result = value << amount;
			return EmitStoreKnown64(rd, static_cast<u32>(result), static_cast<u32>(result >> 32));
		}

		if (amount == 0)
		{
			if (rd == rt)
				return true;

			unsigned rt_low;
			unsigned rt_high;
			return EmitGpr64ReadOperands(rt, HOST_TMP0, HOST_TMP1, &rt_low, &rt_high) &&
				   EmitStoreGpr64(rd, rt_low, rt_high);
		}
		if (amount >= 32)
		{
			unsigned rt_low;
			if (!EmitGprLowOperand(rt, HOST_TMP0, &rt_low))
				return false;

			const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
			const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
			if (amount == 32)
			{
				return m_code.EmitMovRegShiftImm(high_result, rt_low, VitaA32::ShiftType::LSL, 0) &&
					   m_code.EmitMovImm8(low_result, 0) &&
					   EmitStoreGpr64(rd, low_result, high_result);
			}

			return m_code.EmitMovRegShiftImm(high_result, rt_low, VitaA32::ShiftType::LSL,
					   static_cast<u8>(amount - 32)) &&
				   m_code.EmitMovImm8(low_result, 0) &&
				   EmitStoreGpr64(rd, low_result, high_result);
		}

		unsigned rt_low;
		unsigned rt_high;
		if (!EmitGpr64ReadOperands(rt, HOST_TMP0, HOST_TMP1, &rt_low, &rt_high))
			return false;

		const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
		const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
		if (!m_code.EmitMovRegShiftImm(HOST_TMP2, rt_low, VitaA32::ShiftType::LSR,
				static_cast<u8>(32 - amount)) ||
			!m_code.EmitOrrRegShiftImm(high_result, HOST_TMP2, rt_high, VitaA32::ShiftType::LSL,
				static_cast<u8>(amount)) ||
			!m_code.EmitMovRegShiftImm(low_result, rt_low, VitaA32::ShiftType::LSL,
				static_cast<u8>(amount)))
		{
			return false;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuShift64FusedMergeFastPaths++;
#endif
		return EmitStoreGpr64(rd, low_result, high_result);
	}

	bool BlockCompiler::EmitShift64RightImmediate(u32 op, unsigned amount, bool arithmetic)
	{
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::DSRL()/DSRL32()/DSRA()/DSRA32().
		// Shifting register zero writes zero for every immediate amount.
		if (rt == 0)
			return EmitStoreGprZero64(rd);

		u32 rt_low_value = 0;
		u32 rt_high_value = 0;
		if (FindGprPinHost(rt) < 0 && TryGetKnownGpr64(rt, &rt_low_value, &rt_high_value))
		{
			// PCSX2's x86 constant register file folds DSRL/DSRL32/DSRA/DSRA32
			// as full low-64 values; arithmetic shifts need the exact sign word.
			const u64 value = (static_cast<u64>(rt_high_value) << 32) | rt_low_value;
			const u64 result = arithmetic ?
				static_cast<u64>(static_cast<s64>(value) >> amount) :
				(value >> amount);
			return EmitStoreKnown64(rd, static_cast<u32>(result), static_cast<u32>(result >> 32));
		}

		const VitaA32::ShiftType high_shift = arithmetic ? VitaA32::ShiftType::ASR : VitaA32::ShiftType::LSR;
		if (amount == 0)
		{
			if (rd == rt)
				return true;

			unsigned rt_low;
			unsigned rt_high;
			return EmitGpr64ReadOperands(rt, HOST_TMP0, HOST_TMP1, &rt_low, &rt_high) &&
				   EmitStoreGpr64(rd, rt_low, rt_high);
		}
		if (amount >= 32)
		{
			unsigned rt_high;
			if (!EmitGprWordOperand(rt, 1, HOST_TMP1, &rt_high))
				return false;

			const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
			const bool low_ok = amount == 32 ?
				m_code.EmitMovRegShiftImm(low_result, rt_high, VitaA32::ShiftType::LSL, 0) :
				m_code.EmitMovRegShiftImm(low_result, rt_high, high_shift, static_cast<u8>(amount - 32));
			if (!low_ok)
				return false;

			return arithmetic ? EmitStoreGprSignExtended32FromLow(rd, low_result) :
								EmitStoreGprZeroExtended32FromLow(rd, low_result);
		}

		unsigned rt_low;
		unsigned rt_high;
		if (!EmitGpr64ReadOperands(rt, HOST_TMP0, HOST_TMP1, &rt_low, &rt_high))
			return false;

		const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
		const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
		if (!m_code.EmitMovRegShiftImm(HOST_TMP2, rt_high, VitaA32::ShiftType::LSL,
				static_cast<u8>(32 - amount)) ||
			!m_code.EmitOrrRegShiftImm(low_result, HOST_TMP2, rt_low, VitaA32::ShiftType::LSR,
				static_cast<u8>(amount)) ||
			!m_code.EmitMovRegShiftImm(high_result, rt_high, high_shift, static_cast<u8>(amount)))
		{
			return false;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuShift64FusedMergeFastPaths++;
#endif
		return EmitStoreGpr64(rd, low_result, high_result);
	}

	bool BlockCompiler::EmitShift64LeftVariable(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::DSLLV(). The shift amount is
		// irrelevant when the source register is zero.
		if (rt == 0)
			return EmitStoreGprZero64(rd);
		// Register zero supplies a shift amount of 0.
		if (rs == 0 && rd == rt)
			return true;

		u32 rs_value = 0;
		if (TryGetKnownGprLow(rs, &rs_value))
		{
			// PCSX2 x86/ix86-32/iR5900Shift.cpp::recDSLLV_consts() uses the
			// immediate 64-bit shift path for a known RS and dynamic RT.
			const bool emitted = EmitShift64LeftImmediate(op, rs_value & 0x3f);
#if defined(VITASX2_QEMU_VALIDATION)
			if (emitted)
				g_qemuKnownVariableShiftImmediateFastPaths++;
#endif
			return emitted;
		}

		if (rs == 0)
		{
			unsigned rt_low;
			unsigned rt_high;
			return EmitGpr64ReadOperands(rt, HOST_TMP0, HOST_TMP1, &rt_low, &rt_high) &&
				   EmitStoreGpr64(rd, rt_low, rt_high);
		}

		unsigned rs_host;
		if (!EmitLoadGpr64(rt, HOST_TMP0, HOST_TMP1) ||
			!EmitGprLowOperand(rs, HOST_TMP2, &rs_host) ||
			!m_code.EmitAndImm8(HOST_TMP2, rs_host, 0x3f) ||
			!EmitCmpImm32OrReg(HOST_TMP2, 32, HOST_TMP3))
		{
			return false;
		}

		const size_t ge32_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
		if (ge32_branch == static_cast<size_t>(-1))
			return false;

		const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
		const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
		if (!m_code.EmitRsbImm32(HOST_TMP3, HOST_TMP2, 32) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP4, HOST_TMP0, VitaA32::ShiftType::LSR, HOST_TMP3) ||
			!m_code.EmitOrrRegShiftReg(high_result, HOST_TMP4, HOST_TMP1,
				VitaA32::ShiftType::LSL, HOST_TMP2) ||
			!m_code.EmitMovRegShiftReg(low_result, HOST_TMP0, VitaA32::ShiftType::LSL, HOST_TMP2))
		{
			return false;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuShift64FusedMergeFastPaths++;
#endif

		const size_t done_branch = m_code.EmitBranchPlaceholder();
		if (done_branch == static_cast<size_t>(-1))
			return false;

		const size_t ge32_target = m_code.Size();
		if (!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 0x1f) ||
			!m_code.EmitMovRegShiftReg(high_result, HOST_TMP0, VitaA32::ShiftType::LSL, HOST_TMP2) ||
			!m_code.EmitMovImm8(low_result, 0))
		{
			return false;
		}

		const size_t store_target = m_code.Size();
		return m_code.PatchBranch(ge32_branch, ge32_target, VitaA32::Condition::CS) &&
			   m_code.PatchBranch(done_branch, store_target) &&
			   EmitStoreGpr64(rd, low_result, high_result);
	}

	bool BlockCompiler::EmitShift64RightVariable(u32 op, bool arithmetic)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::DSRLV()/DSRAV(). The shift amount
		// is irrelevant when the source register is zero.
		if (rt == 0)
			return EmitStoreGprZero64(rd);
		// Register zero supplies a shift amount of 0.
		if (rs == 0 && rd == rt)
			return true;

		u32 rs_value = 0;
		if (TryGetKnownGprLow(rs, &rs_value))
		{
			// PCSX2 x86/ix86-32/iR5900Shift.cpp::recDSRLV_consts() and
			// recDSRAV_consts() use the matching immediate 64-bit shift path.
			const bool emitted = EmitShift64RightImmediate(op, rs_value & 0x3f, arithmetic);
#if defined(VITASX2_QEMU_VALIDATION)
			if (emitted)
				g_qemuKnownVariableShiftImmediateFastPaths++;
#endif
			return emitted;
		}

		if (rs == 0)
		{
			unsigned rt_low;
			unsigned rt_high;
			return EmitGpr64ReadOperands(rt, HOST_TMP0, HOST_TMP1, &rt_low, &rt_high) &&
				   EmitStoreGpr64(rd, rt_low, rt_high);
		}

		unsigned rs_host;
		if (!EmitLoadGpr64(rt, HOST_TMP0, HOST_TMP1) ||
			!EmitGprLowOperand(rs, HOST_TMP2, &rs_host) ||
			!m_code.EmitAndImm8(HOST_TMP2, rs_host, 0x3f) ||
			!EmitCmpImm32OrReg(HOST_TMP2, 32, HOST_TMP3))
		{
			return false;
		}

		const size_t ge32_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
		if (ge32_branch == static_cast<size_t>(-1))
			return false;

		const VitaA32::ShiftType high_shift = arithmetic ? VitaA32::ShiftType::ASR : VitaA32::ShiftType::LSR;
		const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
		const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
		if (!m_code.EmitRsbImm32(HOST_TMP3, HOST_TMP2, 32) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP4, HOST_TMP1, VitaA32::ShiftType::LSL, HOST_TMP3) ||
			!m_code.EmitOrrRegShiftReg(low_result, HOST_TMP4, HOST_TMP0,
				VitaA32::ShiftType::LSR, HOST_TMP2) ||
			!m_code.EmitMovRegShiftReg(high_result, HOST_TMP1, high_shift, HOST_TMP2))
		{
			return false;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuShift64FusedMergeFastPaths++;
#endif

		const size_t done_branch = m_code.EmitBranchPlaceholder();
		if (done_branch == static_cast<size_t>(-1))
			return false;

		const size_t ge32_target = m_code.Size();
		if (!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 0x1f) ||
			!m_code.EmitMovRegShiftReg(low_result, HOST_TMP1, high_shift, HOST_TMP2) ||
			!(arithmetic ?
				 m_code.EmitMovRegShiftImm(high_result, HOST_TMP1, VitaA32::ShiftType::ASR, 31) :
				 m_code.EmitMovImm8(high_result, 0)))
		{
			return false;
		}

		const size_t store_target = m_code.Size();
		return m_code.PatchBranch(ge32_branch, ge32_target, VitaA32::Condition::CS) &&
			   m_code.PatchBranch(done_branch, store_target) &&
			   EmitStoreGpr64(rd, low_result, high_result);
	}

	bool BlockCompiler::EmitConditionalMove(u32 op, bool move_on_zero)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 x86/ix86-32/iR5900Move.cpp::recMOVZ()/recMOVN() skip the
		// self-copy and fold GPR_IS_CONST1 RT predicates before emitting a test.
		if (rs == rd)
			return true;

		const auto emit_source_copy = [this, rs, rd]() {
			const int rd_low_pin = FindGprPinHost(rd);
			const int rd_high_pin = FindGprPinHighHost(rd);
			const int rs_low_pin = FindGprPinHost(rs);
			const int rs_high_pin = FindGprPinHighHost(rs);
			bool qcache_store = false;
			if (!TryEmitStoreGprLow64FromQCache(rd, rs, &qcache_store))
				return false;
			if (qcache_store)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuConditionalMoveUnconditionalQCacheCopies++;
#endif
				return true;
			}

			if (rd_low_pin >= 0 && rd_high_pin >= 0 && (rs_low_pin < 0 || rs_high_pin < 0))
			{
				const unsigned low_result = SelectGprLowResultHost(rd, HOST_TMP0);
				const unsigned high_result = SelectGprHighResultHost(rd, HOST_TMP1);
				return EmitLoadGpr64(rs, low_result, high_result) &&
					   EmitStoreGpr64(rd, low_result, high_result);
			}

			unsigned rs_low;
			unsigned rs_high;
			return EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high) &&
				   EmitStoreGpr64(rd, rs_low, rs_high);
		};

		u32 rt_low_value = 0;
		u32 rt_high_value = 0;
		if (TryGetKnownGpr64(rt, &rt_low_value, &rt_high_value))
		{
			const bool rt_zero = rt_low_value == 0 && rt_high_value == 0;
			const bool move = move_on_zero ? rt_zero : !rt_zero;
			if (!move)
				return true;

			u32 rs_low_value = 0;
			u32 rs_high_value = 0;
			if (FindGprPinHost(rs) < 0 && TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value))
				return EmitStoreKnown64(rd, rs_low_value, rs_high_value);

			return emit_source_copy();
		}

		unsigned rt_low;
		unsigned rt_high;
		if (!EmitGpr64ReadOperands(rt, HOST_TMP2, HOST_TMP3, &rt_low, &rt_high) ||
			!m_code.EmitOrrReg(HOST_TMP4, rt_low, rt_high, true))
		{
			return false;
		}

		const VitaA32::Condition move_condition = move_on_zero ? VitaA32::Condition::EQ : VitaA32::Condition::NE;
		const int rd_pin_index = FindGprPinIndex(rd);
		const int rs_low_pin = FindGprPinHost(rs);
		const int rs_high_pin = FindGprPinHighHost(rs);
		if (m_dirty_pins_enabled && rd_pin_index >= 0 &&
			m_pin_high_host[rd_pin_index] != NO_GPR_PIN_HOST &&
			m_pin_dirty_low[rd_pin_index] && m_pin_dirty_high[rd_pin_index])
		{
			// PCSX2 recMOVZtemp_()/recMOVNtemp_() use CMOV for both register and
			// memory sources. A dirty dword pin already pays its exit flush, so
			// predicated A32 moves or loads remove the unpredictable branch without
			// adding a false-condition guest-state store.
			const unsigned rd_low_host = m_pin_host[rd_pin_index];
			const unsigned rd_high_host = m_pin_high_host[rd_pin_index];
			bool predicated_copy = false;
			if (rs_low_pin >= 0 && rs_high_pin >= 0)
			{
				if (!m_code.EmitMovRegShiftImm(rd_low_host,
						static_cast<unsigned>(rs_low_pin), VitaA32::ShiftType::LSL, 0,
						false, move_condition) ||
					!m_code.EmitMovRegShiftImm(rd_high_host,
						static_cast<unsigned>(rs_high_pin), VitaA32::ShiftType::LSL, 0,
						false, move_condition))
	{
					return false;
	}
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuConditionalMovePredicatedRegisterCopies++;
#endif
				predicated_copy = true;
			}
			else
			{
				u32 rs_low_value = 0;
				u32 rs_high_value = 0;
				const bool rs_known = TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value);
				const int rs_qcache = FindGprQCache(rs);
				if (rs_qcache >= 0)
	{
					if (!EmitMoveQWordLaneToCore(rd_low_host,
							static_cast<unsigned>(rs_qcache), 0, move_condition) ||
						!EmitMoveQWordLaneToCore(rd_high_host,
							static_cast<unsigned>(rs_qcache), 1, move_condition))
	{
						return false;
	}
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuConditionalMovePredicatedQCacheCopies++;
#endif
					predicated_copy = true;
	}
				else if (rs_known && IsSingleInstructionA32MoveConstant(rs_low_value) &&
					IsSingleInstructionA32MoveConstant(rs_high_value))
	{
					if (!m_code.EmitMovImm32(rd_low_host, rs_low_value, move_condition) ||
						!m_code.EmitMovImm32(rd_high_host, rs_high_value, move_condition))
	{
						return false;
	}
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuConditionalMovePredicatedKnownCopies++;
#endif
					predicated_copy = true;
	}
				else if (rs_low_pin >= 0 && rs_high_pin < 0)
	{
					const size_t offset = GprOffset(rs);
					if (!m_code.EmitMovRegShiftImm(rd_low_host,
							static_cast<unsigned>(rs_low_pin), VitaA32::ShiftType::LSL, 0,
							false, move_condition) ||
						!m_code.EmitLdrImm12(rd_high_host, HOST_CPU_REGS,
							static_cast<u16>(offset + sizeof(u32)), move_condition))
	{
						return false;
	}
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuConditionalMovePredicatedMixedCopies++;
#endif
					predicated_copy = true;
	}
				else if (rs_low_pin < 0)
	{
					const size_t offset = GprOffset(rs);
					if (offset <= 0xff && CanUseA32DualTransferPair(rd_low_host, rd_high_host))
	{
						if (!m_code.EmitLdrdImm8(rd_low_host, rd_high_host, HOST_CPU_REGS,
								static_cast<u8>(offset), move_condition))
						{
							return false;
						}
#if defined(VITASX2_QEMU_VALIDATION)
						g_qemuConditionalMovePredicatedBackingCopies++;
#endif
						predicated_copy = true;
	}
	}
			}

			if (predicated_copy)
			{
				if (!TryDeferGprPinLowStore(rd) || !TryDeferGprPinHighStore(rd))
					return false;

				InvalidateGprQCacheForGuest(rd);
				return true;
			}
		}

		if (rd_pin_index < 0)
		{
			const size_t offset = GprOffset(rd);
			if (rs_low_pin >= 0 && rs_high_pin >= 0 && offset <= 0xff &&
				CanUseA32DualTransferPair(static_cast<unsigned>(rs_low_pin),
					static_cast<unsigned>(rs_high_pin)))
			{
				if (!m_code.EmitStrdImm8(static_cast<unsigned>(rs_low_pin),
						static_cast<unsigned>(rs_high_pin), HOST_CPU_REGS,
						static_cast<u8>(offset), move_condition))
	{
					return false;
	}
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuConditionalMovePredicatedRegisterStores++;
#endif
				InvalidateGprQCacheForGuest(rd);
				return true;
			}

			const int rs_qcache = FindGprQCache(rs);
			if (rs_qcache >= 0 &&
				m_code.EmitVstrDImm(static_cast<unsigned>(rs_qcache) * 2,
					HOST_CPU_REGS, static_cast<u16>(offset), move_condition))
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuConditionalMovePredicatedQCacheStores++;
#endif
				InvalidateGprQCacheForGuest(rd);
				return true;
			}
		}

		const VitaA32::Condition skip_condition = move_on_zero ? VitaA32::Condition::NE : VitaA32::Condition::EQ;
		const size_t skip_store = m_code.EmitBranchPlaceholder(skip_condition);
		if (skip_store == static_cast<size_t>(-1))
			return false;

		if (!emit_source_copy())
			return false;

		return m_code.PatchBranch(skip_store, m_code.Size(), skip_condition);
	}

	bool BlockCompiler::EmitJump(u32 pc, bool link)
	{
		// J/JAL always exit through static direct-link metadata; the branch flag
		// is only consumed by conditional and likely branch tails.
		if (!link)
			return true;

		// PCSX2 owner: Interpreter.cpp::JAL() applies _SetLink(31) before
		// doBranch() executes the delay slot, so a delay-slot write to ra wins.
		return EmitLink(31, pc);
	}

	bool BlockCompiler::EmitRegisterJump(u32 op, u32 pc, bool link)
	{
		const unsigned rs = RS(op);
		const unsigned rd = RD(op);

		// PCSX2 owners: Interpreter.cpp::JR()/JALR() and
		// x86/ix86-32/iR5900Jump.cpp::recJR()/recJALR(). The target is snapped
		// before the delay slot, and JALR links before the delay slot.
		if (!EmitLoadGprLow(rs, HOST_BRANCH_TARGET))
			return false;
		if (EmuConfig.Gamefixes.GoemonTlbHack && !EmitGoemonTranslateHostReg(HOST_BRANCH_TARGET))
			return false;

		if (!link || rd == 0)
			return true;

		return EmitLink(rd, pc);
	}

	bool BlockCompiler::EmitGoemonBlockStartHook(u32 start_pc)
	{
		if (!EmuConfig.Gamefixes.GoemonTlbHack)
			return true;

		if (start_pc == GOEMON_PRELOAD_RETURN_PC_0 || start_pc == GOEMON_PRELOAD_RETURN_PC_1)
		{
			// PCSX2 owners: Interpreter.cpp::JR() and
			// x86/ix86-32/iR5900.cpp::recRecompile() preload Goemon's TLB cache
			// when execution reaches either return PC of the TLB-populating
			// function at 0x356250.
			return m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&GoemonPreloadTlb));
		}

		if (start_pc == GOEMON_UNLOAD_ENTRY_PC)
		{
			// PCSX2 owners: Interpreter.cpp::JAL() and
			// x86/ix86-32/iR5900.cpp::recRecompile() unload a Goemon TLB cache
			// entry at function 0x3563b8. The x86 path also marks the rec cache
			// for reset; Vita requests the same reset and performs it after this
			// generated block returns to the provider loop.
			return m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaRequestA32EeCacheReset)) &&
				   EmitLoadGprLow(4, HOST_TMP0) &&
				   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&GoemonUnloadTlb));
		}

		return true;
	}

	bool BlockCompiler::EmitGoemonTranslateHostReg(unsigned host_reg)
	{
		// PCSX2 owner: x86/ix86-32/iR5900Jump.cpp::recJR()/recJALR() snapshot
		// the register target, translate it with vtlb_DynV2P(), and only then
		// compile the delay slot.
		return m_code.EmitMovRegShiftImm(HOST_TMP0, host_reg, VitaA32::ShiftType::LSL, 0) &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vtlb_V2P)) &&
			   m_code.EmitMovRegShiftImm(host_reg, HOST_TMP0, VitaA32::ShiftType::LSL, 0);
	}

	bool BlockCompiler::EmitLink(unsigned guest_reg, u32 pc)
	{
		return EmitStoreKnownZeroExtended32(guest_reg, pc + 8);
	}

	bool BlockCompiler::EmitCompareGpr64WithKnownForBranch(unsigned guest_reg, u32 low, u32 high)
	{
		unsigned guest_low;
		unsigned guest_high;
		return EmitGpr64ReadOperands(guest_reg, HOST_TMP0, HOST_TMP1, &guest_low, &guest_high) &&
			   EmitCmpImm32OrReg(guest_high, high, HOST_TMP2) &&
			   EmitCmpImm32OrReg(guest_low, low, HOST_TMP2, VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitCompareGpr64ForBranch(unsigned lhs_guest_reg, unsigned rhs_guest_reg)
	{
		// PCSX2 owners: Interpreter.cpp::BEQ()/BNE()/BEQL()/BNEL(). Branch
		// equality observes the low 64-bit SD[0] value; folding $zero only
		// changes how the A32 flags are produced.
		if (lhs_guest_reg == rhs_guest_reg)
			return m_code.EmitCmpReg(HOST_TMP0, HOST_TMP0);

		if (lhs_guest_reg == 0 || rhs_guest_reg == 0)
		{
			const unsigned nonzero_guest_reg = (lhs_guest_reg == 0) ? rhs_guest_reg : lhs_guest_reg;
			unsigned low_host;
			unsigned high_host;
			return EmitGpr64ReadOperands(nonzero_guest_reg, HOST_TMP0, HOST_TMP1, &low_host, &high_host) &&
				   m_code.EmitOrrReg(HOST_TMP0, low_host, high_host, true);
		}

		u32 lhs_low_value = 0;
		u32 lhs_high_value = 0;
		u32 rhs_low_value = 0;
		u32 rhs_high_value = 0;
		const bool lhs_known = TryGetKnownGpr64(lhs_guest_reg, &lhs_low_value, &lhs_high_value);
		const bool rhs_known = TryGetKnownGpr64(rhs_guest_reg, &rhs_low_value, &rhs_high_value);
		if (lhs_known && FindGprPinHost(lhs_guest_reg) < 0)
		{
			// PCSX2 x86/ix86-32/iR5900Branch.cpp::recBEQ()/recBNE() can
			// compare one runtime GPR against one GPR_IS_CONST value. Do the
			// same only when the constant side is not already resident in a pin.
			return EmitCompareGpr64WithKnownForBranch(rhs_guest_reg, lhs_low_value, lhs_high_value);
		}
		if (rhs_known && FindGprPinHost(rhs_guest_reg) < 0)
		{
			return EmitCompareGpr64WithKnownForBranch(lhs_guest_reg, rhs_low_value, rhs_high_value);
		}

		unsigned lhs_low;
		unsigned rhs_low;
		unsigned lhs_high;
		unsigned rhs_high;
		return EmitGpr64ReadOperands(lhs_guest_reg, HOST_TMP0, HOST_TMP1, &lhs_low, &lhs_high) &&
			   EmitGpr64ReadOperands(rhs_guest_reg, HOST_TMP2, HOST_TMP3, &rhs_low, &rhs_high) &&
			   m_code.EmitCmpReg(lhs_low, rhs_low) &&
			   m_code.EmitCmpReg(lhs_high, rhs_high, VitaA32::Condition::EQ);
	}

	bool BlockCompiler::TryEvaluateConstantBranch(u32 op, bool* taken) const
	{
		if (!taken)
			return false;

		const auto evaluate_signed = [](s64 value, SignedBranchCondition condition) {
			switch (condition)
			{
				case SignedBranchCondition::LessThanZero:
					return value < 0;
				case SignedBranchCondition::GreaterEqualZero:
					return value >= 0;
				case SignedBranchCondition::LessEqualZero:
					return value <= 0;
				case SignedBranchCondition::GreaterThanZero:
					return value > 0;
			}
			return false;
		};

		const auto evaluate_known_signed = [&](unsigned guest_reg, SignedBranchCondition condition) {
			if (guest_reg == 0)
			{
				*taken = evaluate_signed(0, condition);
				return true;
			}

			u32 low = 0;
			u32 high = 0;
			if (!TryGetKnownGpr64(guest_reg, &low, &high))
				return false;

			const u64 value = (static_cast<u64>(high) << 32) | low;
			*taken = evaluate_signed(static_cast<s64>(value), condition);
			return true;
		};

		switch (op >> 26)
		{
			case 0x01:
				// PCSX2 owner: x86/ix86-32/iR5900Branch.cpp folds non-link
				// GPR_IS_CONST1 REGIMM predicates, including BLTZL and BGEZL.
				switch (RT(op))
	{
					case 0x00: // BLTZ
					case 0x02: // BLTZL
						return evaluate_known_signed(RS(op), SignedBranchCondition::LessThanZero);
					case 0x01: // BGEZ
					case 0x03: // BGEZL
						return evaluate_known_signed(RS(op), SignedBranchCondition::GreaterEqualZero);
					default:
						return false;
	}

			case 0x04: // BEQ
			case 0x14: // BEQL
			case 0x05: // BNE
			case 0x15: // BNEL
			{
				const unsigned rs = RS(op);
				const unsigned rt = RT(op);
				const bool branch_on_equal = (op >> 26) == 0x04 || (op >> 26) == 0x14;
				if (rs == rt)
	{
					*taken = branch_on_equal;
					return true;
	}

				u32 rs_low = 0;
				u32 rs_high = 0;
				u32 rt_low = 0;
				u32 rt_high = 0;
				if (!TryGetKnownGpr64(rs, &rs_low, &rs_high) ||
					!TryGetKnownGpr64(rt, &rt_low, &rt_high))
	{
					return false;
	}

				const bool equal = rs_low == rt_low && rs_high == rt_high;
				*taken = (equal == branch_on_equal);
				return true;
			}

			case 0x06: // BLEZ
			case 0x16: // BLEZL
				return evaluate_known_signed(RS(op), SignedBranchCondition::LessEqualZero);
			case 0x07: // BGTZ
			case 0x17: // BGTZL
				return evaluate_known_signed(RS(op), SignedBranchCondition::GreaterThanZero);
			default:
				return false;
		}
	}

	bool BlockCompiler::TryEvaluateConstantRegimmLinkBranch(u32 op, bool* taken) const
	{
		if (!taken || (op >> 26) != 0x01)
			return false;

		const bool branch_on_less = [&]() {
			switch (RT(op))
			{
				case 0x10: // BLTZAL
				case 0x12: // BLTZALL
					return true;
				case 0x11: // BGEZAL
				case 0x13: // BGEZALL
					return false;
				default:
					return false;
			}
		}();

		switch (RT(op))
		{
			case 0x10:
			case 0x11:
			case 0x12:
			case 0x13:
				break;
			default:
				return false;
		}

		const unsigned rs = RS(op);
		if (rs == 31)
		{
			// PCSX2 owners: Interpreter.cpp::BLTZAL()/BGEZAL()/BLTZALL()/
			// BGEZALL() and x86/ix86-32/iR5900Branch.cpp::rec*AL() store ra
			// before testing RS. EmitLink() writes a zero high word, so ra is
			// non-negative for the signed low64 predicate.
			*taken = !branch_on_less;
			return true;
		}

		if (rs == 0)
		{
			*taken = !branch_on_less;
			return true;
		}

		u32 low = 0;
		u32 high = 0;
		if (!TryGetKnownGpr64(rs, &low, &high))
			return false;

		const s64 value = static_cast<s64>((static_cast<u64>(high) << 32) | low);
		*taken = branch_on_less ? value < 0 : value >= 0;
		return true;
	}

	bool BlockCompiler::EmitBranchEqual(u32 op, bool branch_on_equal)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (m_forwarded_boolean_branch && !branch_on_equal &&
			((rs == m_forwarded_boolean_guest && rt == 0) ||
				(rt == m_forwarded_boolean_guest && rs == 0)))
		{
			// PCSX2's MODE_READ mapping lets recBNE() consume the preceding
			// SLT/SLTU boolean without a GPR-file round-trip. The forwarded value
			// is already the normalized zero/nonzero predicate required by BNE.
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuForwardedBooleanBranchLoadsElided++;
			g_qemuForwardedBooleanBranchNormalizationsElided += 3;
#endif
			return true;
		}

		if (rs == rt)
			return m_code.EmitMovImm8(m_branch_flag_host, branch_on_equal ? 1 : 0);

		u32 rs_low = 0;
		u32 rs_high = 0;
		u32 rt_low = 0;
		u32 rt_high = 0;
		if (TryGetKnownGpr64(rs, &rs_low, &rs_high) && TryGetKnownGpr64(rt, &rt_low, &rt_high))
		{
			// PCSX2 x86/ix86-32/iR5900Branch.cpp::recBEQ_const() /
			// recBNE_const() resolve the branch predicate from GPR_IS_CONST2.
			const bool equal = rs_low == rt_low && rs_high == rt_high;
			return m_code.EmitMovImm8(m_branch_flag_host, (equal == branch_on_equal) ? 1 : 0);
		}

		return EmitCompareGpr64ForBranch(rs, rt) &&
			   m_code.EmitMovImm8(m_branch_flag_host, 0) &&
			   m_code.EmitMovImm8(m_branch_flag_host, 1, branch_on_equal ? VitaA32::Condition::EQ : VitaA32::Condition::NE);
	}

	bool BlockCompiler::EmitBranchSigned(u32 op, SignedBranchCondition condition)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);

		// PCSX2 owners: Interpreter.cpp::BLTZ/BGEZ/BLEZ/BGTZ and likely/link
		// variants compare the low 64-bit SD[0] value. $zero makes those
		// predicates compile-time constants.
		if (rs == 0)
		{
			const bool taken = condition == SignedBranchCondition::GreaterEqualZero ||
							   condition == SignedBranchCondition::LessEqualZero;
			return m_code.EmitMovImm8(m_branch_flag_host, taken ? 1 : 0);
		}

		const bool regimm_link_reads_ra = (op >> 26) == 0x01 && rs == 31 &&
			(rt == 0x10 || rt == 0x11 || rt == 0x12 || rt == 0x13);
		u32 rs_low_value = 0;
		u32 rs_high_value = 0;
		if (!regimm_link_reads_ra && TryGetKnownGpr64(rs, &rs_low_value, &rs_high_value))
		{
			// PCSX2 x86/ix86-32/iR5900Branch.cpp folds GPR_IS_CONST1 signed
			// branches. Require exact low64 state because these predicates read
			// SD[0], not only the low word.
			const s64 value = static_cast<s64>((static_cast<u64>(rs_high_value) << 32) | rs_low_value);
			bool taken = false;
			switch (condition)
			{
				case SignedBranchCondition::LessThanZero:
					taken = value < 0;
					break;
				case SignedBranchCondition::GreaterEqualZero:
					taken = value >= 0;
					break;
				case SignedBranchCondition::LessEqualZero:
					taken = value <= 0;
					break;
				case SignedBranchCondition::GreaterThanZero:
					taken = value > 0;
					break;
			}
			return m_code.EmitMovImm8(m_branch_flag_host, taken ? 1 : 0);
		}

		if (condition == SignedBranchCondition::LessThanZero ||
			condition == SignedBranchCondition::GreaterEqualZero)
		{
			unsigned rs_high;
			if (!EmitGprWordOperand(rs, 1, HOST_TMP1, &rs_high) ||
				!m_code.EmitMovRegShiftImm(m_branch_flag_host, rs_high,
					VitaA32::ShiftType::LSR, 31))
			{
				return false;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuSignedBranchSignBitFastPaths++;
#endif
			return condition == SignedBranchCondition::LessThanZero ||
				m_code.EmitEorImm8(m_branch_flag_host, m_branch_flag_host, 1);
		}

		unsigned rs_low;
		unsigned rs_high;
		if (!EmitGpr64ReadOperands(rs, HOST_TMP0, HOST_TMP1, &rs_low, &rs_high) ||
			!m_code.EmitCmpImm32(rs_low, 1) ||
			!m_code.EmitSbcImm8(m_branch_flag_host, rs_high, 0, true) ||
			!m_code.EmitMovImm8(m_branch_flag_host, 0) ||
			!m_code.EmitMovImm8(m_branch_flag_host, 1,
				condition == SignedBranchCondition::LessEqualZero ?
					VitaA32::Condition::LT : VitaA32::Condition::GE))
		{
			return false;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuSignedBranchOneCompareFastPaths++;
#endif
		return true;
	}

	bool BlockCompiler::EmitCop1Branch(u32 op)
	{
		// PCSX2 owner: FPU.cpp::BC1F()/BC1T()/BC1FL()/BC1TL() branch on
		// FCR31.C; x86/iFPU.cpp::REC_FPUBRANCH() flushes FPU state before
		// entering the shared branch path. Vita helper-backed scalar COP1 ops
		// already commit through fpuRegs before a BC1 block observes FCR31.
		const unsigned rt = RT(op);
		const bool branch_on_true = rt == 0x01 || rt == 0x03;

		return m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
			   m_code.EmitTstImm32(HOST_TMP1, FPU_FCR31_CONDITION_FLAG) &&
			   m_code.EmitMovImm8(m_branch_flag_host, 0) &&
			   m_code.EmitMovImm8(m_branch_flag_host, 1,
				   branch_on_true ? VitaA32::Condition::NE : VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitCop2Branch(u32 op)
	{
		// PCSX2 owners: COP2.cpp::BC2F()/BC2T()/BC2FL()/BC2TL() branch on
		// ((VU0.VI[REG_VPU_STAT].US[0] >> 8) & 1). x86/microVU_Macro.inl
		// tests the same bit through VU0.VI[REG_VPU_STAT].UL & 0x100.
		const unsigned rt = RT(op);
		const bool branch_on_true = rt == 0x01 || rt == 0x03;

		return EmitVu0ViAddress(HOST_TMP0, VU0_REG_VPU_STAT) &&
			   m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0) &&
			   m_code.EmitTstImm32(HOST_TMP1, 0x100u) &&
			   m_code.EmitMovImm8(m_branch_flag_host, 0) &&
			   m_code.EmitMovImm8(m_branch_flag_host, 1,
				   branch_on_true ? VitaA32::Condition::NE : VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitCop0Branch(u32 op)
	{
		// PCSX2 owners: COP0.cpp::CPCOND0()/BC0F()/BC0T()/BC0FL()/BC0TL() and
		// x86/iCOP0.cpp::_setupBranchTest(). The branch condition is:
		// (((DMAC_STAT.CIS | ~DMAC_PCR.CPC) & 0x3ff) == 0x3ff).
		const unsigned rt = RT(op);
		const bool branch_on_true = rt == 0x01 || rt == 0x03;
		const u32 dmac_regs_addr = static_cast<u32>(reinterpret_cast<uptr>(&eeHw[DMAC_REGS_HW_OFFSET]));

		return m_code.EmitMovImm32(HOST_TMP0, dmac_regs_addr) &&
			   m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, DMAC_PCR_DMAC_OFFSET) &&
			   m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP0, DMAC_STAT_DMAC_OFFSET) &&
			   m_code.EmitBicRegShiftImm(HOST_TMP1, HOST_TMP1, HOST_TMP2, VitaA32::ShiftType::LSL, 0) &&
			   m_code.EmitMovImm32(HOST_TMP2, DMAC_CPCOND_MASK) &&
			   m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP2, true) &&
			   m_code.EmitMovImm8(m_branch_flag_host, 0) &&
			   m_code.EmitMovImm8(m_branch_flag_host, 1,
				   branch_on_true ? VitaA32::Condition::EQ : VitaA32::Condition::NE);
	}

	bool BlockCompiler::EmitSetLessThan64(unsigned guest_reg, bool signed_compare,
		unsigned lhs_low, unsigned lhs_high, unsigned rhs_low, unsigned rhs_high)
	{
		if (guest_reg == 0)
			return true;

		const bool forward_to_branch = IsForwardedBooleanBranchResult(guest_reg);
		const bool direct_result = FindGprPinHost(guest_reg) >= 0;
		const unsigned result_reg = forward_to_branch ? m_branch_flag_host :
			(direct_result ? SelectGprLowResultHost(guest_reg, HOST_TMP4) : HOST_TMP4);
		const auto commit_result = [&]() {
			if (!forward_to_branch)
				return EmitStoreGprZeroExtended32FromLow(guest_reg, result_reg);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuForwardedBooleanBranchStoresElided += 2;
#endif
			// The exact sequential-SQ resident form establishes the zero high word
			// at canonical/page translation and after its only helper seam. Preserve
			// that iCore-style mapping across same-page self-links instead of
			// rematerializing the invariant at every producer.
			if (m_resident_vtlb_qword_pointer)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				if (!m_emitting_deferred_resident_event_suffix)
					g_qemuResidentForwardedBooleanHighZeroHotInstructionsElided++;
#endif
				return true;
			}

			// Other forwarded forms still need to establish the complete
			// zero-extended guest value at their producer.
			return m_code.EmitMovImm8(HOST_TMP5, 0);
		};
		if (!signed_compare)
		{
			if (forward_to_branch && m_resident_forwarded_boolean_mask)
			{
				// PCSX2 iCore keeps the branch-only MODE_WRITE result private. A32's
				// borrow is already the inverse of unsigned less-than, so materialize
				// the transient as 0/-1 with one SBC from the resident zero high word.
				// Publication seams normalize its low bit back to architectural 0/1.
				if (!m_code.EmitCmpReg(lhs_high, rhs_high) ||
					!m_code.EmitCmpReg(lhs_low, rhs_low, VitaA32::Condition::EQ) ||
					!m_code.EmitSbcReg(result_reg, HOST_TMP5, HOST_TMP5) ||
					!commit_result())
				{
					return false;
				}
#if defined(VITASX2_QEMU_VALIDATION)
				if (!m_emitting_deferred_resident_event_suffix)
					g_qemuResidentForwardedBooleanMaskHotInstructionsElided++;
#endif
				return true;
			}
			if (direct_result)
			{
				return m_code.EmitCmpReg(lhs_high, rhs_high) &&
					   m_code.EmitCmpReg(lhs_low, rhs_low, VitaA32::Condition::EQ) &&
					   m_code.EmitMovImm8(result_reg, 0) &&
					   m_code.EmitMovImm8(result_reg, 1, VitaA32::Condition::CC) &&
					   commit_result();
			}

			return m_code.EmitMovImm8(result_reg, 0) &&
				   m_code.EmitCmpReg(lhs_high, rhs_high) &&
				   m_code.EmitCmpReg(lhs_low, rhs_low, VitaA32::Condition::EQ) &&
				   m_code.EmitMovImm8(result_reg, 1, VitaA32::Condition::CC) &&
				   commit_result();
		}

		// Compare the low word first to feed its borrow into the high-word SBCS.
		// N xor V after that high subtraction is the signed 64-bit predicate,
		// avoiding the old high-equal and join branches on Cortex-A9.
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuSigned64CompareCarryChains++;
#endif
		return m_code.EmitCmpReg(lhs_low, rhs_low) &&
			   m_code.EmitSbcReg(result_reg, lhs_high, rhs_high, true) &&
			   m_code.EmitMovImm8(result_reg, 0) &&
			   m_code.EmitMovImm8(result_reg, 1, VitaA32::Condition::LT) &&
			   commit_result();
	}

	bool BlockCompiler::EmitSetLessThan64Known(unsigned guest_reg, bool signed_compare,
		unsigned runtime_guest_reg, u32 known_low, u32 known_high, bool known_is_lhs)
	{
		if (guest_reg == 0)
			return true;

		const bool direct_result = FindGprPinHost(guest_reg) >= 0;
		const unsigned result_reg = direct_result ? SelectGprLowResultHost(guest_reg, HOST_TMP4) : HOST_TMP4;
		unsigned runtime_low = HOST_TMP0;
		unsigned runtime_high = HOST_TMP1;
		if (!EmitGpr64ReadOperands(runtime_guest_reg, HOST_TMP0, HOST_TMP1, &runtime_low, &runtime_high))
			return false;

		// PCSX2 x86/ix86-32/iR5900Arit.cpp::recSLTs_const() lowers both
		// recSLT_consts()/constt() and recSLTU_consts()/constt() to one 64-bit
		// compare followed by SETL/SETB (or the reversed predicate). Preserve the
		// low-word carry through SBCS/RSC here so A32 also avoids control flow.
#if defined(VITASX2_QEMU_VALIDATION)
		if (signed_compare)
			g_qemuSigned64CompareCarryChains++;
		else
			g_qemuUnsignedKnown64CompareCarryChains++;
#endif
		bool compare_ok;
		if (known_is_lhs)
		{
			compare_ok = m_code.EmitRsbImm32(result_reg, runtime_low, known_low, true);
			if (!compare_ok)
			{
				compare_ok = m_code.EmitMovImm32(HOST_TMP2, known_low) &&
					m_code.EmitCmpReg(HOST_TMP2, runtime_low);
			}
			if (!compare_ok)
				return false;

			compare_ok = m_code.EmitRscImm32(result_reg, runtime_high, known_high, true);
			if (!compare_ok)
			{
				compare_ok = m_code.EmitMovImm32(HOST_TMP2, known_high) &&
					m_code.EmitSbcReg(result_reg, HOST_TMP2, runtime_high, true);
			}
		}
		else
		{
			compare_ok = EmitCmpImm32OrReg(runtime_low, known_low, HOST_TMP2);
			if (compare_ok && known_high == 0)
				compare_ok = m_code.EmitSbcImm8(result_reg, runtime_high, 0, true);
			else if (compare_ok && known_high == 0xffffffffu)
				compare_ok = m_code.EmitAdcImm8(result_reg, runtime_high, 0, true);
			else if (compare_ok)
			{
				compare_ok = m_code.EmitSbcImm32(result_reg, runtime_high, known_high, true);
#if defined(VITASX2_QEMU_VALIDATION)
				if (compare_ok)
					g_qemuCarryModifiedImmediateFastPaths++;
#endif
				if (!compare_ok)
	{
					compare_ok = m_code.EmitMovImm32(HOST_TMP2, known_high) &&
						m_code.EmitSbcReg(result_reg, runtime_high, HOST_TMP2, true);
	}
			}
		}

		return compare_ok &&
			m_code.EmitMovImm8(result_reg, 0) &&
			m_code.EmitMovImm8(result_reg, 1,
				signed_compare ? VitaA32::Condition::LT : VitaA32::Condition::CC) &&
			EmitStoreGprZeroExtended32FromLow(guest_reg, result_reg);
	}

	bool BlockCompiler::EmitSetLessThan64Imm(unsigned guest_reg, s32 imm, bool signed_compare,
		unsigned lhs_low, unsigned lhs_high)
	{
		if (guest_reg == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLTI()/SLTIU(). The immediate is
		// sign-extended to 64 bits, then compared as signed or unsigned.
		const u32 imm_low = static_cast<u32>(imm);
		const u32 imm_high = (imm < 0) ? 0xffffffffu : 0u;
		const bool direct_result = FindGprPinHost(guest_reg) >= 0;
		const unsigned result_reg = direct_result ? SelectGprLowResultHost(guest_reg, HOST_TMP4) : HOST_TMP4;
		if (!signed_compare)
		{
			if ((!direct_result && !m_code.EmitMovImm8(HOST_TMP4, 0)) ||
				!m_code.EmitCmpImm32(lhs_high, imm_high) ||
				!(m_code.EmitCmpImm32(lhs_low, imm_low, VitaA32::Condition::EQ) ||
					(m_code.EmitMovImm32(HOST_TMP2, imm_low) &&
						m_code.EmitCmpReg(lhs_low, HOST_TMP2, VitaA32::Condition::EQ))) ||
				(direct_result && !m_code.EmitMovImm8(result_reg, 0)) ||
				!m_code.EmitMovImm8(result_reg, 1, VitaA32::Condition::CC))
			{
				return false;
			}

			return EmitStoreGprZeroExtended32FromLow(guest_reg, result_reg);
		}

		// The sign-extended high word is either zero or all ones. For the latter,
		// high - 0xffffffff - borrow is high + carry, so ADCS preserves the same
		// N/V signed predicate without materializing the high immediate.
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuSigned64CompareCarryChains++;
#endif
		return EmitCmpImm32OrReg(lhs_low, imm_low, HOST_TMP2) &&
			   (imm_high == 0 ? m_code.EmitSbcImm8(result_reg, lhs_high, 0, true) :
								m_code.EmitAdcImm8(result_reg, lhs_high, 0, true)) &&
			   m_code.EmitMovImm8(result_reg, 0) &&
			   m_code.EmitMovImm8(result_reg, 1, VitaA32::Condition::LT) &&
			   EmitStoreGprZeroExtended32FromLow(guest_reg, result_reg);
	}

	bool BlockCompiler::EmitPartialWordLoad(u32 op, bool left)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::LWL() / LWR().
		const unsigned rt = RT(op);
		const unsigned result_reg = (rt != 0) ? SelectGprLowResultHost(rt, HOST_TMP0) : HOST_TMP0;
		constexpr u32 LWL_MASK[4] = {0x00ffffffu, 0x0000ffffu, 0x000000ffu, 0x00000000u};
		constexpr u32 LWR_MASK[4] = {0x00000000u, 0xff000000u, 0xffff0000u, 0xffffff00u};
		constexpr u8 LWL_SHIFT[4] = {24, 16, 8, 0};
		constexpr u8 LWR_SHIFT[4] = {0, 8, 16, 24};
		const auto emit_full_load = [this, rt, result_reg]() {
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuPartialWordFullLoadFastPaths++;
#endif
			return m_code.EmitLdrImm12(result_reg, HOST_TMP0, 0) &&
				   EmitStoreGprSignExtended32FromLow(rt, result_reg);
		};
		const auto emit_zero_load_skip_counter = []() -> bool {
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuPartialZeroLoadSkips++;
#endif
			return true;
		};

		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address & ~3u, HOST_TMP0,
				KnownVtlbFastPathKind::Partial))
		{
			const unsigned lane = known_address & 3u;
			const u8 shift = left ? LWL_SHIFT[lane] : LWR_SHIFT[lane];
			const u32 mask = left ? LWL_MASK[lane] : LWR_MASK[lane];
			if (rt == 0)
				return emit_zero_load_skip_counter();

			if (rt != 0 && ((left && lane == 3) || (!left && lane == 0)))
				return emit_full_load();

			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP0, 0))
				return false;

			if (rt != 0)
			{
				if (!EmitLoadGprLow(rt, HOST_TMP1) ||
					!(mask == 0 ? m_code.EmitMovImm8(HOST_TMP1, 0) :
								   EmitAndImm32OrReg(HOST_TMP1, HOST_TMP1, mask, HOST_TMP2)) ||
					!(shift == 0 ? true :
						(left ? m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL, shift) :
								m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSR, shift))) ||
					!m_code.EmitOrrReg(result_reg, HOST_TMP1, HOST_TMP0))
	{
					return false;
	}

				if (left || lane == 0)
	{
					if (!EmitStoreGprSignExtended32FromLow(rt, result_reg))
						return false;
	}
				else if (!EmitLoadGprHigh(rt, HOST_TMP1) ||
						 !EmitStoreGpr64(rt, result_reg, HOST_TMP1))
	{
					return false;
	}
			}

			return true;
		}

		size_t handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks dirty_pins;
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitAndImm8(HOST_TMP3, HOST_TMP0, 3) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP3, VitaA32::ShiftType::LSL, 3))
		{
			return false;
		}

		if (left && !m_code.EmitRsbImm32(HOST_TMP3, HOST_TMP3, 24))
		{
			return false;
		}

		if (!m_code.EmitBicImm32(HOST_TMP0, HOST_TMP0, 3) ||
			!EmitVtlbNonHandlerHostAddress(
				HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback, &dirty_pins))
		{
			return false;
		}

		if (rt == 0)
		{
			if (!emit_zero_load_skip_counter())
				return false;

			PartialMemoryColdTail tail{
				handler_fallback,
				m_code.Size(),
				left ? PartialMemoryOp::WordLoadLeft : PartialMemoryOp::WordLoadRight,
				rt,
			};
			tail.dirty_pins = dirty_pins;
			m_partial_memory_cold_tails.push_back(tail);
			return true;
		}

		size_t full_lane_branch = static_cast<size_t>(-1);
		if (rt != 0)
		{
			if (!m_code.EmitCmpImm32(HOST_TMP3, 0))
				return false;
			full_lane_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (full_lane_branch == static_cast<size_t>(-1))
				return false;
		}

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP0, 0))
			return false;

		if (rt != 0)
		{
			if (left)
			{
				if (!EmitLoadGprLow(rt, HOST_TMP1) ||
					!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP3, 32) ||
					!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) ||
					!m_code.EmitAndRegShiftReg(HOST_TMP1, HOST_TMP1, HOST_TMP2,
						VitaA32::ShiftType::LSR, HOST_TMP4) ||
					!m_code.EmitOrrRegShiftReg(result_reg, HOST_TMP1, HOST_TMP0,
						VitaA32::ShiftType::LSL, HOST_TMP3) ||
					!EmitStoreGprSignExtended32FromLow(rt, result_reg))
	{
					return false;
	}
			}
			else
			{
				if (!EmitLoadGprLow(rt, HOST_TMP1) ||
					!EmitLoadGprHigh(rt, HOST_TMP2) ||
					!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP3, 32) ||
					!m_code.EmitMovImm32(HOST_TMP5, 0xffffffffu) ||
					!m_code.EmitAndRegShiftReg(HOST_TMP1, HOST_TMP1, HOST_TMP5,
						VitaA32::ShiftType::LSL, HOST_TMP4) ||
					!m_code.EmitOrrRegShiftReg(result_reg, HOST_TMP1, HOST_TMP0,
						VitaA32::ShiftType::LSR, HOST_TMP3) ||
					!EmitStoreGpr64(rt, result_reg, HOST_TMP2))
	{
					return false;
	}
			}
		}

		if (full_lane_branch != static_cast<size_t>(-1))
		{
			const size_t general_done = m_code.EmitBranchPlaceholder();
			if (general_done == static_cast<size_t>(-1))
				return false;

			const size_t full_lane_target = m_code.Size();
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuPartialWordFullLoadFastPaths++;
#endif
			if (!m_code.PatchBranch(full_lane_branch, full_lane_target, VitaA32::Condition::EQ) ||
				!m_code.EmitLdrImm12(result_reg, HOST_TMP0, 0) ||
				!EmitStoreGprSignExtended32FromLow(rt, result_reg) ||
				!m_code.PatchBranch(general_done, m_code.Size()))
			{
				return false;
			}
		}

		PartialMemoryColdTail tail{
			handler_fallback,
			m_code.Size(),
			left ? PartialMemoryOp::WordLoadLeft : PartialMemoryOp::WordLoadRight,
			rt,
		};
		tail.dirty_pins = dirty_pins;
		m_partial_memory_cold_tails.push_back(tail);
		return true;
	}

	bool BlockCompiler::EmitPartialWordStore(u32 op, bool left)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::SWL() / SWR().
		const unsigned rt = RT(op);
		constexpr u32 SWL_MASK[4] = {0xffffff00u, 0xffff0000u, 0xff000000u, 0x00000000u};
		constexpr u32 SWR_MASK[4] = {0x00000000u, 0x000000ffu, 0x0000ffffu, 0x00ffffffu};
		constexpr u8 SWL_SHIFT[4] = {24, 16, 8, 0};
		constexpr u8 SWR_SHIFT[4] = {0, 8, 16, 24};
		const auto emit_full_store = [this, rt](unsigned address_reg) {
			const int cached_qreg = FindGprQCache(rt);
			if (cached_qreg >= 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDirectMemoryStores++;
#endif
				return EmitStoreQWordLane(static_cast<unsigned>(cached_qreg), 0,
					address_reg, 0, HOST_TMP1);
			}

#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuPartialWordFullStoreFastPaths++;
#endif
			return EmitLoadPartialStoreLowValue(rt, HOST_TMP1) &&
				   m_code.EmitStrImm12(HOST_TMP1, address_reg, 0);
		};

		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address & ~3u, HOST_TMP5,
				KnownVtlbFastPathKind::Partial))
		{
			const unsigned lane = known_address & 3u;
			const u8 shift = left ? SWL_SHIFT[lane] : SWR_SHIFT[lane];
			const u32 mask = left ? SWL_MASK[lane] : SWR_MASK[lane];
			if ((left && lane == 3) || (!left && lane == 0))
				return emit_full_store(HOST_TMP5);

			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP5, 0) ||
				!(mask == 0 ? m_code.EmitMovImm8(HOST_TMP0, 0) :
							   EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, mask, HOST_TMP2)) ||
				!EmitLoadPartialStoreLowValue(rt, HOST_TMP1) ||
				!(shift == 0 ? true :
					(left ? m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSR, shift) :
							m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, shift))) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_TMP5, 0))
			{
				return false;
			}

			return true;
		}

		size_t handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks dirty_pins;
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitAndImm8(HOST_TMP3, HOST_TMP0, 3) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP3, VitaA32::ShiftType::LSL, 3))
		{
			return false;
		}

		if (left && !m_code.EmitRsbImm32(HOST_TMP3, HOST_TMP3, 24))
		{
			return false;
		}

		if (!m_code.EmitBicImm32(HOST_TMP0, HOST_TMP0, 3) ||
			!EmitVtlbNonHandlerHostAddress(
				HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback, &dirty_pins))
		{
			return false;
		}

		if (!m_code.EmitCmpImm32(HOST_TMP3, 0))
			return false;
		const size_t full_lane_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (full_lane_branch == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP5, 0) ||
			!EmitLoadPartialStoreLowValue(rt, HOST_TMP1) ||
			!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP3, 32) ||
			!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) ||
			!(left ? m_code.EmitAndRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP2,
						   VitaA32::ShiftType::LSL, HOST_TMP4) :
						 m_code.EmitAndRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP2,
						   VitaA32::ShiftType::LSR, HOST_TMP4)) ||
			!(left ? m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP1,
						   VitaA32::ShiftType::LSR, HOST_TMP3) :
						 m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP1,
						   VitaA32::ShiftType::LSL, HOST_TMP3)) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_TMP5, 0))
		{
			return false;
		}

		const size_t general_done = m_code.EmitBranchPlaceholder();
		if (general_done == static_cast<size_t>(-1))
			return false;

		const size_t full_lane_target = m_code.Size();
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuPartialWordFullStoreFastPaths++;
#endif
		if (!m_code.PatchBranch(full_lane_branch, full_lane_target, VitaA32::Condition::EQ) ||
			!emit_full_store(HOST_TMP0) ||
			!m_code.PatchBranch(general_done, m_code.Size()))
		{
			return false;
		}

		PartialMemoryColdTail tail{
			handler_fallback,
			m_code.Size(),
			left ? PartialMemoryOp::WordStoreLeft : PartialMemoryOp::WordStoreRight,
			rt,
		};
		tail.dirty_pins = dirty_pins;
		CapturePartialStoreValue(&tail);
		m_partial_memory_cold_tails.push_back(tail);
		return true;
	}

	bool BlockCompiler::EmitPartialDwordLoad(u32 op, bool left)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::LDL() / LDR().
		const unsigned rt = RT(op);
		const auto emit_full_load = [this, rt]() {
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuPartialDwordFullLoadFastPaths++;
#endif
			return m_code.EmitLdrdImm8(HOST_TMP2, HOST_TMP3, HOST_TMP0, 0) &&
				   EmitStoreGpr64(rt, HOST_TMP2, HOST_TMP3);
		};
		const auto emit_zero_load_skip_counter = []() -> bool {
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuPartialZeroLoadSkips++;
#endif
			return true;
		};
		const bool register_merge = rt != 0 && FindGprPinHost(rt) >= 0;
		const auto low_bits_mask = [](unsigned bits) -> u32 {
			return bits == 0 ? 0u : (bits >= 32 ? 0xffffffffu : ((1u << bits) - 1u));
		};
		const auto emit_apply_preserve_and_or =
			[this](unsigned dest, u32 preserve_mask, unsigned loaded, bool loaded_zero) -> bool {
				if (preserve_mask == 0)
	{
					if (loaded_zero)
						return m_code.EmitMovImm8(dest, 0);
					return m_code.EmitMovRegShiftImm(dest, loaded, VitaA32::ShiftType::LSL, 0);
	}

				if (preserve_mask != 0xffffffffu &&
					!EmitAndImm32OrReg(dest, dest, preserve_mask, HOST_TMP2))
	{
					return false;
	}

				return loaded_zero || m_code.EmitOrrReg(dest, dest, loaded);
			};
		const auto emit_pinned_merge_load = [&](unsigned shift) -> bool {
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuPartialDwordMergedLoadFastPaths++;
#endif
			if (!m_code.EmitLdrdImm8(HOST_TMP2, HOST_TMP3, HOST_TMP0, 0) ||
				!EmitLoadGpr64Value(rt, HOST_TMP0, HOST_TMP1))
			{
				return false;
			}

			u32 low_preserve = 0;
			u32 high_preserve = 0;
			bool loaded_low_zero = false;
			bool loaded_high_zero = false;
			if (left)
			{
				const unsigned shift_bits = (7 - shift) * 8;
				low_preserve = low_bits_mask(shift_bits);
				high_preserve = shift_bits <= 32 ? 0u : low_bits_mask(shift_bits - 32);

				if (shift_bits < 32)
	{
					if (!m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP2, VitaA32::ShiftType::LSL,
							static_cast<u8>(shift_bits)) ||
						!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP3, VitaA32::ShiftType::LSL,
							static_cast<u8>(shift_bits)) ||
						!m_code.EmitOrrRegShiftImm(HOST_TMP5, HOST_TMP5, HOST_TMP2,
							VitaA32::ShiftType::LSR, static_cast<u8>(32 - shift_bits)))
	{
						return false;
	}
	}
				else if (shift_bits == 32)
	{
					loaded_low_zero = true;
					if (!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP2, VitaA32::ShiftType::LSL, 0))
						return false;
	}
				else
	{
					loaded_low_zero = true;
					if (!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP2, VitaA32::ShiftType::LSL,
							static_cast<u8>(shift_bits - 32)))
	{
						return false;
	}
	}
			}
			else
			{
				const unsigned shift_bits = shift * 8;
				const unsigned loaded_bits = 64 - shift_bits;
				low_preserve = loaded_bits >= 32 ? 0u : (0xffffffffu << loaded_bits);
				high_preserve = shift_bits >= 32 ? 0xffffffffu : (0xffffffffu << (32 - shift_bits));

				if (shift_bits < 32)
	{
					if (!m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP2, VitaA32::ShiftType::LSR,
							static_cast<u8>(shift_bits)) ||
						!m_code.EmitOrrRegShiftImm(HOST_TMP4, HOST_TMP4, HOST_TMP3,
							VitaA32::ShiftType::LSL, static_cast<u8>(32 - shift_bits)) ||
						!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP3, VitaA32::ShiftType::LSR,
							static_cast<u8>(shift_bits)))
	{
						return false;
	}
	}
				else if (shift_bits == 32)
	{
					if (!m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP3, VitaA32::ShiftType::LSL, 0))
						return false;
					loaded_high_zero = true;
	}
				else
	{
					if (!m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP3, VitaA32::ShiftType::LSR,
							static_cast<u8>(shift_bits - 32)))
	{
						return false;
	}
					loaded_high_zero = true;
	}
			}

			return emit_apply_preserve_and_or(HOST_TMP0, low_preserve, HOST_TMP4, loaded_low_zero) &&
				   emit_apply_preserve_and_or(HOST_TMP1, high_preserve, HOST_TMP5, loaded_high_zero) &&
				   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
		};
		const auto emit_runtime_pinned_merge_load = [&]() -> bool {
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuPartialDwordMergedLoadFastPaths++;
#endif
			const auto emit_left_below_32 = [&]() -> bool {
				return m_code.EmitRsbImm32(HOST_TMP5, HOST_TMP4, 32) &&
					   m_code.EmitMovRegShiftReg(HOST_TMP1, HOST_TMP3,
						   VitaA32::ShiftType::LSL, HOST_TMP4) &&
					   m_code.EmitOrrRegShiftReg(HOST_TMP1, HOST_TMP1, HOST_TMP2,
						   VitaA32::ShiftType::LSR, HOST_TMP5) &&
					   m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_TMP2,
						   VitaA32::ShiftType::LSL, HOST_TMP4) &&
					   m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) &&
					   m_code.EmitAndRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP2,
						   VitaA32::ShiftType::LSR, HOST_TMP5) &&
					   m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP3);
			};
			const auto emit_left_above_32 = [&]() -> bool {
				return m_code.EmitSubImm8(HOST_TMP4, HOST_TMP4, 32) &&
					   m_code.EmitRsbImm32(HOST_TMP5, HOST_TMP4, 32) &&
					   m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_TMP2,
						   VitaA32::ShiftType::LSL, HOST_TMP4) &&
					   m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) &&
					   m_code.EmitAndRegShiftReg(HOST_TMP1, HOST_TMP1, HOST_TMP2,
						   VitaA32::ShiftType::LSR, HOST_TMP5) &&
					   m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP3);
			};
			const auto emit_right_below_32 = [&]() -> bool {
				return m_code.EmitRsbImm32(HOST_TMP5, HOST_TMP4, 32) &&
					   m_code.EmitMovRegShiftReg(HOST_TMP0, HOST_TMP2,
						   VitaA32::ShiftType::LSR, HOST_TMP4) &&
					   m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP3,
						   VitaA32::ShiftType::LSL, HOST_TMP5) &&
					   m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_TMP3,
						   VitaA32::ShiftType::LSR, HOST_TMP4) &&
					   m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) &&
					   m_code.EmitAndRegShiftReg(HOST_TMP1, HOST_TMP1, HOST_TMP2,
						   VitaA32::ShiftType::LSL, HOST_TMP5) &&
					   m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP3);
			};
			const auto emit_right_above_32 = [&]() -> bool {
				return m_code.EmitSubImm8(HOST_TMP4, HOST_TMP4, 32) &&
					   m_code.EmitRsbImm32(HOST_TMP5, HOST_TMP4, 32) &&
					   m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_TMP3,
						   VitaA32::ShiftType::LSR, HOST_TMP4) &&
					   m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) &&
					   m_code.EmitAndRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP2,
						   VitaA32::ShiftType::LSL, HOST_TMP5) &&
					   m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP3);
			};

			if (!(left ? m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP3, 7) :
						 m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP3, VitaA32::ShiftType::LSL, 0)) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP4, VitaA32::ShiftType::LSL, 3) ||
				!m_code.EmitLdrdImm8(HOST_TMP2, HOST_TMP3, HOST_TMP0, 0) ||
				!EmitLoadGpr64Value(rt, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitCmpImm32(HOST_TMP4, 32))
			{
				return false;
			}

			const size_t below_32 = m_code.EmitBranchPlaceholder(VitaA32::Condition::CC);
			const size_t equal_32 = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (below_32 == static_cast<size_t>(-1) || equal_32 == static_cast<size_t>(-1))
				return false;

			if (!(left ? emit_left_above_32() : emit_right_above_32()))
				return false;

			const size_t done_above = m_code.EmitBranchPlaceholder();
			if (done_above == static_cast<size_t>(-1))
				return false;

			const size_t below_32_target = m_code.Size();
			if (!m_code.PatchBranch(below_32, below_32_target, VitaA32::Condition::CC) ||
				!(left ? emit_left_below_32() : emit_right_below_32()))
			{
				return false;
			}

			const size_t done_below = m_code.EmitBranchPlaceholder();
			if (done_below == static_cast<size_t>(-1))
				return false;

			const size_t equal_32_target = m_code.Size();
			if (!m_code.PatchBranch(equal_32, equal_32_target, VitaA32::Condition::EQ))
				return false;

			if (!(left ? m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP2, VitaA32::ShiftType::LSL, 0) :
						 m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP3, VitaA32::ShiftType::LSL, 0)))
			{
				return false;
			}

			const size_t done_equal = m_code.EmitBranchPlaceholder();
			if (done_equal == static_cast<size_t>(-1))
				return false;

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_above, done_target) &&
				   m_code.PatchBranch(done_below, done_target) &&
				   m_code.PatchBranch(done_equal, done_target) &&
				   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
		};

		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address & ~7u, HOST_TMP0,
				KnownVtlbFastPathKind::Partial))
		{
			const unsigned shift = known_address & 7u;
			if (rt == 0)
				return emit_zero_load_skip_counter();

			if (rt != 0)
			{
				if ((left && shift == 7) || (!left && shift == 0))
					return emit_full_load();

				if (register_merge)
					return emit_pinned_merge_load(shift);

				const auto emit_byte = [this, rt](unsigned memory_byte, unsigned dest_byte) {
					return m_code.EmitLdrbImm12(HOST_TMP1, HOST_TMP0, static_cast<u16>(memory_byte)) &&
						   m_code.EmitStrbImm12(HOST_TMP1, HOST_CPU_REGS,
							   static_cast<u16>(GprOffset(rt) + dest_byte));
	};

				if (left)
	{
					for (unsigned memory_byte = 0; memory_byte <= shift; memory_byte++)
	{
						const unsigned dest_byte = 7 - shift + memory_byte;
						if (!emit_byte(memory_byte, dest_byte))
							return false;
	}
	}
				else
	{
					for (unsigned memory_byte = shift; memory_byte < 8; memory_byte++)
	{
						const unsigned dest_byte = memory_byte - shift;
						if (!emit_byte(memory_byte, dest_byte))
							return false;
	}
	}
			}

			return EmitRefreshGprPinFromBacking(rt);
		}

		size_t handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks dirty_pins;
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitAndImm8(HOST_TMP3, HOST_TMP0, 7) ||
			!m_code.EmitBicImm32(HOST_TMP0, HOST_TMP0, 7) ||
			!EmitVtlbNonHandlerHostAddress(
				HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback, &dirty_pins))
		{
			return false;
		}

		if (rt == 0)
		{
			if (!emit_zero_load_skip_counter())
				return false;

			PartialMemoryColdTail tail{
				handler_fallback,
				m_code.Size(),
				left ? PartialMemoryOp::DwordLoadLeft : PartialMemoryOp::DwordLoadRight,
				rt,
			};
			tail.dirty_pins = dirty_pins;
			m_partial_memory_cold_tails.push_back(tail);
			return true;
		}

		if (register_merge)
		{
			if (!m_code.EmitCmpImm32(HOST_TMP3, left ? 7 : 0))
				return false;

			const size_t full_lane_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (full_lane_branch == static_cast<size_t>(-1))
				return false;

			if (!emit_runtime_pinned_merge_load())
				return false;

			const size_t general_done = m_code.EmitBranchPlaceholder();
			if (general_done == static_cast<size_t>(-1))
				return false;

			const size_t full_lane_target = m_code.Size();
			if (!m_code.PatchBranch(full_lane_branch, full_lane_target, VitaA32::Condition::EQ) ||
				!emit_full_load() ||
				!m_code.PatchBranch(general_done, m_code.Size()))
			{
				return false;
			}

			PartialMemoryColdTail tail{
				handler_fallback,
				m_code.Size(),
				left ? PartialMemoryOp::DwordLoadLeft : PartialMemoryOp::DwordLoadRight,
				rt,
			};
			tail.dirty_pins = dirty_pins;
			m_partial_memory_cold_tails.push_back(tail);
			return true;
		}

		const auto emit_byte = [this, rt](unsigned memory_byte, unsigned dest_byte) {
			return m_code.EmitLdrbImm12(HOST_TMP1, HOST_TMP0, static_cast<u16>(memory_byte)) &&
				   m_code.EmitStrbImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(GprOffset(rt) + dest_byte));
		};

		const auto emit_case_body = [&](unsigned shift) {
			if (rt == 0)
				return true;

			if ((left && shift == 7) || (!left && shift == 0))
				return emit_full_load();

			if (left)
			{
				for (unsigned memory_byte = 0; memory_byte <= shift; memory_byte++)
	{
					const unsigned dest_byte = 7 - shift + memory_byte;
					if (!emit_byte(memory_byte, dest_byte))
						return false;
	}
			}
			else
			{
				for (unsigned memory_byte = shift; memory_byte < 8; memory_byte++)
	{
					const unsigned dest_byte = memory_byte - shift;
					if (!emit_byte(memory_byte, dest_byte))
						return false;
	}
			}

			return true;
		};

		size_t case_branches[7]{};
		for (unsigned shift = 0; shift < 7; shift++)
		{
			if (!EmitCmpImm32OrReg(HOST_TMP3, shift, HOST_TMP4))
			{
				return false;
			}

			case_branches[shift] = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (case_branches[shift] == static_cast<size_t>(-1))
				return false;
		}

		size_t done_branches[8]{};
		if (!emit_case_body(7))
			return false;

		done_branches[7] = m_code.EmitBranchPlaceholder();
		if (done_branches[7] == static_cast<size_t>(-1))
			return false;

		for (unsigned shift = 0; shift < 7; shift++)
		{
			const size_t case_target = m_code.Size();
			if (!m_code.PatchBranch(case_branches[shift], case_target, VitaA32::Condition::EQ) ||
				!emit_case_body(shift))
			{
				return false;
			}

			done_branches[shift] = m_code.EmitBranchPlaceholder();
			if (done_branches[shift] == static_cast<size_t>(-1))
				return false;
		}

		const size_t done_target = m_code.Size();
		for (size_t branch : done_branches)
		{
			if (!m_code.PatchBranch(branch, done_target))
				return false;
		}

		if (!register_merge && !EmitRefreshGprPinFromBacking(rt))
			return false;

		const size_t join_offset = m_code.Size();
		PartialMemoryColdTail tail{
			handler_fallback,
			join_offset,
			left ? PartialMemoryOp::DwordLoadLeft : PartialMemoryOp::DwordLoadRight,
			rt,
		};
		tail.dirty_pins = dirty_pins;
		m_partial_memory_cold_tails.push_back(tail);
		return true;
	}

	bool BlockCompiler::EmitPartialDwordStore(u32 op, bool left)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::SDL() / SDR().
		const unsigned rt = RT(op);
		// The byte-lane source reads consume the raw backing slot, so deferred
		// pinned rt words must land in backing first.
		if (!EmitFlushDirtyGprPinsForGuest(rt))
			return false;
		u32 rt_low_value = 0;
		u32 rt_high_value = 0;
		const bool rt64_known =
			rt != 0 && FindGprPinHost(rt) < 0 && TryGetKnownGpr64(rt, &rt_low_value, &rt_high_value);
#if defined(VITASX2_QEMU_VALIDATION)
		if (rt64_known)
			g_qemuGprPartialDwordStoreValueFastPaths++;
#endif
		const auto emit_byte = [this, rt, rt64_known, rt_low_value, rt_high_value]
			(unsigned source_byte, unsigned memory_byte) {
				return EmitLoadPartialDwordStoreByteValue(rt, source_byte, HOST_TMP1,
						   rt64_known, rt_low_value, rt_high_value) &&
					   m_code.EmitStrbImm12(HOST_TMP1, HOST_TMP0, static_cast<u16>(memory_byte));
			};
		const auto emit_full_store = [this, rt]() {
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuPartialDwordFullStoreFastPaths++;
#endif
			return EmitLoadGpr64Value(rt, HOST_TMP2, HOST_TMP3) &&
				   m_code.EmitStrdImm8(HOST_TMP2, HOST_TMP3, HOST_TMP0, 0);
		};

		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address & ~7u, HOST_TMP0,
				KnownVtlbFastPathKind::Partial))
		{
			const unsigned shift = known_address & 7u;
			if (rt != 0 && ((left && shift == 7) || (!left && shift == 0)))
				return emit_full_store();

			if (left)
			{
				for (unsigned memory_byte = 0; memory_byte <= shift; memory_byte++)
	{
					const unsigned source_byte = 7 - shift + memory_byte;
					if (!emit_byte(source_byte, memory_byte))
						return false;
	}
			}
			else
			{
				for (unsigned memory_byte = shift; memory_byte < 8; memory_byte++)
	{
					const unsigned source_byte = memory_byte - shift;
					if (!emit_byte(source_byte, memory_byte))
						return false;
	}
			}

			return true;
		}

		size_t handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks dirty_pins;
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitAndImm8(HOST_TMP3, HOST_TMP0, 7) ||
			!m_code.EmitBicImm32(HOST_TMP0, HOST_TMP0, 7) ||
			!EmitVtlbNonHandlerHostAddress(
				HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback, &dirty_pins))
		{
			return false;
		}

		const auto emit_case_body = [&](unsigned shift) {
			if (rt != 0 && ((left && shift == 7) || (!left && shift == 0)))
				return emit_full_store();

			if (left)
			{
				for (unsigned memory_byte = 0; memory_byte <= shift; memory_byte++)
	{
					const unsigned source_byte = 7 - shift + memory_byte;
					if (!emit_byte(source_byte, memory_byte))
						return false;
	}
			}
			else
			{
				for (unsigned memory_byte = shift; memory_byte < 8; memory_byte++)
	{
					const unsigned source_byte = memory_byte - shift;
					if (!emit_byte(source_byte, memory_byte))
						return false;
	}
			}

			return true;
		};

		size_t case_branches[7]{};
		for (unsigned shift = 0; shift < 7; shift++)
		{
			if (!EmitCmpImm32OrReg(HOST_TMP3, shift, HOST_TMP4))
			{
				return false;
			}

			case_branches[shift] = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (case_branches[shift] == static_cast<size_t>(-1))
				return false;
		}

		size_t done_branches[8]{};
		if (!emit_case_body(7))
			return false;

		done_branches[7] = m_code.EmitBranchPlaceholder();
		if (done_branches[7] == static_cast<size_t>(-1))
			return false;

		for (unsigned shift = 0; shift < 7; shift++)
		{
			const size_t case_target = m_code.Size();
			if (!m_code.PatchBranch(case_branches[shift], case_target, VitaA32::Condition::EQ) ||
				!emit_case_body(shift))
			{
				return false;
			}

			done_branches[shift] = m_code.EmitBranchPlaceholder();
			if (done_branches[shift] == static_cast<size_t>(-1))
				return false;
		}

		const size_t done_target = m_code.Size();
		for (size_t branch : done_branches)
		{
			if (!m_code.PatchBranch(branch, done_target))
				return false;
		}

		PartialMemoryColdTail tail{
			handler_fallback,
			done_target,
			left ? PartialMemoryOp::DwordStoreLeft : PartialMemoryOp::DwordStoreRight,
			rt,
		};
		tail.dirty_pins = dirty_pins;
		CapturePartialStoreValue(&tail);
		m_partial_memory_cold_tails.push_back(tail);
		return true;
	}

	bool BlockCompiler::EmitLoadWithCounterReadEvent(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, const void* read_helper, bool sign_extend,
		bool branch_delay_slot, ScalarLoadWidth width, u8 alignment_mask, bool counter_read_event)
	{
		const unsigned rt = RT(op);
		const unsigned address_reg = (width == ScalarLoadWidth::Dword) ? HOST_TMP2 : HOST_TMP0;
		const unsigned vmap_reg = HOST_TMP1;
		const unsigned scratch_reg = (width == ScalarLoadWidth::Dword) ? HOST_TMP0 : HOST_TMP2;
		const unsigned result_reg = (rt != 0 && width != ScalarLoadWidth::Dword) ?
										SelectGprLowResultHost(rt, HOST_TMP0) :
										HOST_TMP0;

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks dirty_pins;
		const auto emit_load_from_host = [&]() -> bool {
			switch (width)
			{
				case ScalarLoadWidth::Byte:
					return sign_extend ? m_code.EmitLdrsbImm8(result_reg, HOST_TMP0, 0) :
										 m_code.EmitLdrbImm12(result_reg, HOST_TMP0, 0);
				case ScalarLoadWidth::Halfword:
					return sign_extend ? m_code.EmitLdrshImm8(result_reg, HOST_TMP0, 0) :
										 m_code.EmitLdrhImm8(result_reg, HOST_TMP0, 0);
				case ScalarLoadWidth::Word:
					return m_code.EmitLdrImm12(result_reg, HOST_TMP0, 0);
				case ScalarLoadWidth::Dword:
					// PCSX2 owner: R5900OpcodeImpl.cpp::LD() via vtlb_memRead64().
					// Keep the translated address out of r0/r1 so Cortex-A9 can use
					// one LDRD instead of two dependent scalar loads.
					return m_code.EmitLdrdImm8(HOST_TMP0, HOST_TMP1, address_reg, 0);
			}

			return false;
		};
		const auto emit_store_result = [&]() -> bool {
			if (width == ScalarLoadWidth::Dword)
			{
				if (rt == 0)
	{
					// PCSX2 owner: R5900OpcodeImpl.cpp::LD() writes cpuRegs.GPR.r[_Rt_]
					// directly. This is intentionally different from LQ's gpr_GetWritePtr().
					const size_t offset = GprOffset(0);
					return m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(offset)) &&
						   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32))) &&
						   EmitRefreshRawGpr0KnownZeroFromLow64(HOST_TMP0, HOST_TMP1);
	}

				return EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
			}

			if (rt == 0)
				return true;

			return sign_extend ? EmitStoreGprSignExtended32FromLow(rt, result_reg) :
								 EmitStoreGprZeroExtended32FromLow(rt, result_reg);
		};
		const bool skip_zero_load_result = rt == 0 && width != ScalarLoadWidth::Dword;
		const auto emit_zero_load_skip_counter = []() -> bool {
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuScalarZeroLoadSkips++;
#endif
			return true;
		};
		if (m_compatible_vtlb_pointer_access &&
			pc == m_gpr_link_signature.vtlb_pointer.access_pc &&
			width == ScalarLoadWidth::Word && sign_extend &&
			op == memRead32(m_gpr_link_signature.vtlb_pointer.access_pc) &&
			m_compatible_vtlb_pointer_unaligned_fallback != static_cast<size_t>(-1) &&
			m_compatible_vtlb_pointer_handler_fallback != static_cast<size_t>(-1))
		{
			if (!m_code.EmitLdrImm12PostIndex(result_reg,
					GprLinkSignature::VTLB_POINTER_HOST,
					m_gpr_link_signature.vtlb_pointer.stride) ||
				!emit_store_result())
			{
				return false;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuCompatibleVtlbPointerPostIncrementLoads++;
#endif
			m_scalar_load_cold_tails.push_back({
				m_compatible_vtlb_pointer_unaligned_fallback,
				m_compatible_vtlb_pointer_handler_fallback,
				m_code.Size(),
				pc,
				raw_cycles_through_instruction,
				event_exit,
				read_helper,
				width,
				static_cast<u8>(rt),
				sign_extend,
				branch_delay_slot,
				counter_read_event,
				GprLinkSignature::VTLB_POINTER_HOST,
				m_compatible_vtlb_pointer_dirty_pins,
			});
			return true;
		}

		u32 known_address = 0;
		if (TryGetKnownEffectiveAddress(op, &known_address) &&
			(alignment_mask == 0 || (known_address & alignment_mask) == 0) &&
			(!counter_read_event || ((known_address & 0xffffe000u) != 0x10000000u)) &&
			TryEmitKnownVtlbNonHandlerHostAddress(known_address, address_reg))
		{
			if (skip_zero_load_result)
				return emit_zero_load_skip_counter();

			return emit_load_from_host() && emit_store_result();
		}

		if (!EmitEffectiveAddress(op, address_reg))
			return false;

		if (alignment_mask != 0)
		{
			if (!m_code.EmitAndImm8(vmap_reg, address_reg, alignment_mask, true))
				return false;

			unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (unaligned_fallback == static_cast<size_t>(-1))
				return false;
		}

		if (!EmitVtlbNonHandlerHostAddress(
				address_reg, vmap_reg, scratch_reg, &handler_fallback, &dirty_pins))
			return false;

		if (skip_zero_load_result)
		{
			if (!emit_zero_load_skip_counter())
				return false;

			m_scalar_load_cold_tails.push_back({
				unaligned_fallback,
				handler_fallback,
				m_code.Size(),
				pc,
				raw_cycles_through_instruction,
				event_exit,
				read_helper,
				width,
				rt,
				sign_extend,
				branch_delay_slot,
				counter_read_event,
				address_reg,
				dirty_pins,
			});
			return true;
		}

		if (!emit_load_from_host() || !emit_store_result())
			return false;

		m_scalar_load_cold_tails.push_back({
			unaligned_fallback,
			handler_fallback,
			m_code.Size(),
			pc,
			raw_cycles_through_instruction,
			event_exit,
			read_helper,
			width,
			rt,
			sign_extend,
			branch_delay_slot,
			counter_read_event,
			address_reg,
			dirty_pins,
		});
		return true;
	}

	bool BlockCompiler::EmitCounterReadFlagFromAddress(unsigned host_reg)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::LB()/LBU()/LH()/LHU()/LW()
		// check (addr & 0xffffe000) == 0x10000000 after the load to force
		// an EE counter-read event test.
		constexpr u32 EE_COUNTER_PAGE_SHIFT = 13;
		constexpr u32 EE_COUNTER_PAGE_TAG = 0x10000000u >> EE_COUNTER_PAGE_SHIFT;
		return m_code.EmitMovRegShiftImm(HOST_TMP2, host_reg, VitaA32::ShiftType::LSR, EE_COUNTER_PAGE_SHIFT) &&
			   m_code.EmitCmpImm32(HOST_TMP2, EE_COUNTER_PAGE_TAG) &&
			   m_code.EmitMovImm8(m_branch_flag_host, 0) &&
			   m_code.EmitMovImm8(m_branch_flag_host, 1, VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitCounterReadEventExit(u32 next_pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		if (!event_exit || raw_cycles_through_instruction == 0)
			return false;

		if (!m_code.EmitCmpImm32(m_branch_flag_host, 0))
		{
			return false;
		}

		const size_t not_counter_read = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (not_counter_read == static_cast<size_t>(-1))
			return false;

		const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
		if (!EmitStorePc(next_pc) ||
			!EmitAddScaledCyclesToCpu(cycles) ||
			!EmitEventExitReturn(event_exit))
		{
			return false;
		}

		return m_code.PatchBranch(not_counter_read, m_code.Size(), VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitAddressErrorEventExit(u32 next_pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool store, const GprPinDirtyMasks& dirty_pins)
	{
			if (!event_exit || raw_cycles_through_instruction == 0)
				return false;

			const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
			return EmitSyncGprPinsToBacking(&dirty_pins) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP0, VitaA32::ShiftType::LSL, 0) &&
				   EmitStorePc(next_pc) &&
				   EmitAddScaledCyclesToCpu(cycles) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP4, VitaA32::ShiftType::LSL, 0) &&
			   m_code.EmitMovImm8(HOST_TMP1, store ? 1 : 0) &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeRaiseAddressError)) &&
			   EmitEventExitReturn(event_exit);
	}

	bool BlockCompiler::EmitSystemHelperEventExit(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
		const void* helper, const void* event_exit, bool request_cache_reset)
	{
		if (!helper || !event_exit || raw_cycles_through_instruction == 0)
			return false;

			const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
			if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
				!EmitSyncGprPinsToBacking() ||
				!EmitStorePc(next_pc) ||
				!EmitAddScaledCyclesToCpu(cycles) ||
				!m_code.EmitCallAbsolute(helper))
		{
			return false;
		}

		if (request_cache_reset &&
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaRequestA32EeCacheReset)))
		{
			return false;
		}

		return EmitEventExitReturn(event_exit);
	}

	bool BlockCompiler::FlushColdTails()
	{
		for (const ScalarLoadColdTail& tail : m_scalar_load_cold_tails)
		{
			if (!EmitScalarLoadColdTail(tail))
				return false;
		}

		for (const ScalarStoreColdTail& tail : m_scalar_store_cold_tails)
		{
			if (!EmitScalarStoreColdTail(tail))
				return false;
		}

		for (const QwordLoadColdTail& tail : m_qword_load_cold_tails)
		{
			if (!EmitQwordLoadColdTail(tail))
				return false;
		}

		for (const QwordStoreColdTail& tail : m_qword_store_cold_tails)
		{
			if (!EmitQwordStoreColdTail(tail))
				return false;
		}

		for (const Cop1WordMemoryColdTail& tail : m_cop1_word_memory_cold_tails)
		{
			if (!EmitCop1WordMemoryColdTail(tail))
				return false;
		}

		for (const Cop2QwordMemoryColdTail& tail : m_cop2_qword_memory_cold_tails)
		{
			if (!EmitCop2QwordMemoryColdTail(tail))
				return false;
		}

		for (const Vu0SyncColdTail& tail : m_vu0_sync_cold_tails)
		{
			if (!EmitVu0SyncColdTail(tail))
				return false;
		}

		for (const PartialMemoryColdTail& tail : m_partial_memory_cold_tails)
		{
			if (!EmitPartialMemoryColdTail(tail))
				return false;
		}

		m_scalar_load_cold_tails.clear();
		m_scalar_store_cold_tails.clear();
		m_qword_load_cold_tails.clear();
		m_qword_store_cold_tails.clear();
		m_cop1_word_memory_cold_tails.clear();
		m_cop2_qword_memory_cold_tails.clear();
		m_vu0_sync_cold_tails.clear();
		m_partial_memory_cold_tails.clear();
		return true;
	}

	bool BlockCompiler::EmitScalarLoadColdTail(const ScalarLoadColdTail& tail)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memRead*() /
		// R5900OpcodeImpl.cpp::LB/LBU/LH/LHU/LW/LWU/LD().
		// Handler pages dispatch through PCSX2's vTLB directly; unaligned pages
		// use the generated address-error event path before any memory dispatch.
		// The hot non-handler VTLB path falls through to the next guest instruction.
		if (tail.unaligned_fallback != static_cast<size_t>(-1))
		{
			const size_t address_error_target = m_code.Size();
			if (!m_code.PatchBranch(tail.unaligned_fallback, address_error_target, VitaA32::Condition::NE) ||
				(tail.address_reg != HOST_TMP0 &&
					!m_code.EmitMovRegShiftImm(HOST_TMP0, tail.address_reg, VitaA32::ShiftType::LSL, 0)) ||
				!EmitAddressErrorEventExit(tail.pc + 4, tail.raw_cycles_through_instruction,
					tail.event_exit, false, tail.dirty_pins))
			{
				return false;
			}
		}

		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.handler_fallback, fallback_target, VitaA32::Condition::MI))
			return false;
			if (tail.address_reg != HOST_TMP0 &&
				!m_code.EmitMovRegShiftImm(HOST_TMP0, tail.address_reg, VitaA32::ShiftType::LSL, 0))
			{
				return false;
			}
			if (!EmitSyncGprPinsToBacking(&tail.dirty_pins))
				return false;

			const bool needs_counter_event = tail.counter_read_event && tail.rt != 0;
			const auto emit_normalize_narrow_low = [&]() -> bool {
			switch (tail.width)
			{
				case ScalarLoadWidth::Byte:
					return tail.sign_extend ? m_code.EmitSxtb(HOST_TMP0, HOST_TMP0) :
											  m_code.EmitUbfx(HOST_TMP0, HOST_TMP0, 0, 8);
				case ScalarLoadWidth::Halfword:
					return tail.sign_extend ? m_code.EmitSxth(HOST_TMP0, HOST_TMP0) :
											  m_code.EmitUxth(HOST_TMP0, HOST_TMP0);
				case ScalarLoadWidth::Word:
				case ScalarLoadWidth::Dword:
					return true;
			}

			return false;
		};
		const auto emit_store_result = [&]() -> bool {
			if (tail.width == ScalarLoadWidth::Dword)
			{
				if (tail.rt == 0)
	{
					// PCSX2 owner: R5900OpcodeImpl.cpp::LD() writes cpuRegs.GPR.r[_Rt_]
					// directly. This is intentionally different from LQ's gpr_GetWritePtr().
					const size_t offset = GprOffset(0);
					return m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(offset)) &&
						   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32))) &&
						   EmitRefreshRawGpr0KnownZeroFromLow64(HOST_TMP0, HOST_TMP1);
	}

				return EmitStoreGpr64(tail.rt, HOST_TMP0, HOST_TMP1);
			}

			if (tail.rt == 0)
				return true;

			return tail.sign_extend ? EmitStoreGprSignExtended32FromLow(tail.rt, HOST_TMP0) :
									  EmitStoreGprZeroExtended32FromLow(tail.rt, HOST_TMP0);
		};

		if ((tail.branch_delay_slot && needs_counter_event &&
			 !m_code.EmitMovRegShiftImm(HOST_TMP5, m_branch_flag_host, VitaA32::ShiftType::LSL, 0)) ||
				(needs_counter_event && !EmitCounterReadFlagFromAddress(HOST_TMP0)) ||
				!m_code.EmitCallAbsolute(tail.read_helper) ||
				!EmitStageCompatibleSchedulerCountdown(HOST_TMP2, false) ||
				!EmitReloadGprPinsAfterClobber(static_cast<u16>(
					(1u << HOST_TMP0) | (1u << HOST_TMP1) | (1u << HOST_TMP2) |
					(1u << HOST_TMP3) | (1u << HOST_TMP4) | (1u << HOST_LR))) ||
				(m_compatible_vtlb_pointer &&
				 tail.pc == m_gpr_link_signature.vtlb_pointer.access_pc &&
				 !m_code.EmitMovImm8(GprLinkSignature::VTLB_POINTER_HOST, 0)) ||
				(tail.width != ScalarLoadWidth::Dword && tail.rt != 0 && !emit_normalize_narrow_low()) ||
				!emit_store_result() ||
				!EmitFlushDirtyGprPins())
			{
				return false;
			}
#if defined(VITASX2_QEMU_VALIDATION)
		if (m_compatible_vtlb_pointer &&
			tail.pc == m_gpr_link_signature.vtlb_pointer.access_pc)
		{
			g_qemuCompatibleVtlbPointerColdInvalidationInstructions++;
		}
#endif

		if (needs_counter_event &&
			(!EmitCounterReadEventExit(tail.pc + 4, tail.raw_cycles_through_instruction, tail.event_exit) ||
			 (tail.branch_delay_slot &&
				 !m_code.EmitMovRegShiftImm(m_branch_flag_host, HOST_TMP5, VitaA32::ShiftType::LSL, 0))))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	void BlockCompiler::CaptureScalarStoreValue(ScalarStoreColdTail* tail)
	{
		if (!tail || tail->rt == 0 || FindGprPinHost(tail->rt) >= 0)
			return;

		if (tail->width == ScalarStoreWidth::Dword)
		{
			u32 low = 0;
			u32 high = 0;
			if (TryGetKnownGpr64(tail->rt, &low, &high))
			{
				tail->rt_low_known = true;
				tail->rt_high_known = true;
				tail->rt_low = low;
				tail->rt_high = high;
			}
			return;
		}

		u32 low = 0;
		if (TryGetKnownGprLow(tail->rt, &low))
		{
			tail->rt_low_known = true;
			tail->rt_low = low;
		}
	}

	bool BlockCompiler::EmitScalarStoreColdTail(const ScalarStoreColdTail& tail)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memWrite*() / R5900OpcodeImpl.cpp::SB/SH/SW/SD().
		// Handler pages dispatch through PCSX2's vTLB directly; unaligned pages
		// use the generated address-error event path before any memory dispatch.
		// The non-handler VTLB path falls through after the native store.
		if (tail.unaligned_fallback != static_cast<size_t>(-1))
		{
			const size_t address_error_target = m_code.Size();
			if (!m_code.PatchBranch(tail.unaligned_fallback, address_error_target, VitaA32::Condition::NE) ||
				!EmitAddressErrorEventExit(tail.pc + 4, tail.raw_cycles_through_instruction,
					tail.event_exit, true, tail.dirty_pins))
			{
				return false;
			}
		}

			const size_t fallback_target = m_code.Size();
			if (!m_code.PatchBranch(tail.handler_fallback, fallback_target, VitaA32::Condition::MI))
				return false;
			if (!EmitSyncGprPinsToBacking(&tail.dirty_pins))
				return false;

			switch (tail.width)
		{
			case ScalarStoreWidth::Byte:
			case ScalarStoreWidth::Halfword:
			case ScalarStoreWidth::Word:
				if (!EmitLoadGprLowKnownValue(tail.rt, HOST_TMP1, tail.rt_low_known, tail.rt_low))
					return false;
				break;

			case ScalarStoreWidth::Dword:
				// PCSX2 owner: vtlb.cpp::vtlb_memWrite<mem64_t>() takes
				// (u32 addr, u64 value). AAPCS places the 64-bit value in r2/r3.
				if (!EmitLoadGpr64KnownValue(tail.rt, HOST_TMP2, HOST_TMP3,
						tail.rt_low_known && tail.rt_high_known, tail.rt_low, tail.rt_high))
					return false;
				break;
		}

		if (!m_code.EmitCallAbsolute(tail.write_helper) ||
			!EmitStageCompatibleSchedulerCountdown(HOST_TMP2, false))
			return false;

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitQwordLoadColdTail(const QwordLoadColdTail& tail)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memRead128() / R5900OpcodeImpl.cpp::LQ().
		// Handler-backed pages dispatch through vTLB directly and keep the
		// 128-bit payload in q0; non-handler pages fall through after the native
		// NEON load/store. LQ r0 still performs the handler read for side effects
		// but discards the returned qword like R5900OpcodeImpl.cpp::LQ().
		constexpr unsigned NEON_VALUE = 0;
		InvalidateGprQCacheForQreg(NEON_VALUE);
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.handler_fallback, fallback_target, VitaA32::Condition::MI) ||
			!EmitSyncGprPinsToBacking(&tail.dirty_pins) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vtlb_memRead128)) ||
			!EmitStoreGprQ128(tail.rt, NEON_VALUE, HOST_TMP1))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitQwordStoreColdTail(const QwordStoreColdTail& tail)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memWrite128() / R5900OpcodeImpl.cpp::SQ().
		// Handler-backed pages dispatch through vTLB directly. SQ reads the raw
		// GPR backing slot, including r0, matching R5900OpcodeImpl.cpp::SQ().
		constexpr unsigned NEON_VALUE = 0;
		InvalidateGprQCacheForQreg(NEON_VALUE);
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.handler_fallback, fallback_target, VitaA32::Condition::MI))
		{
			return false;
		}
		if (m_resident_scheduler_countdown)
		{
			const size_t restore_start = m_code.Size();
			if (!EmitRestoreResidentSchedulerCountdown())
				return false;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuResidentSchedulerCountdownHandlerInstructions += static_cast<u32>(
				(m_code.Size() - restore_start) / sizeof(u32));
#endif
		}
		if (!EmitPrepareResidentForwardedBooleanForPreProducerSync() ||
			!EmitSyncGprPinsToBacking(&tail.dirty_pins, true))
			return false;
		if (m_resident_vtlb_qword_pointer && tail.rt == 0 &&
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP3,
				VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}
		if (!EmitLoadCpuRegsQ128(GprOffset(tail.rt), NEON_VALUE, HOST_TMP1) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vtlb_memWrite128)))
		{
			return false;
		}
		if (m_resident_vtlb_qword_pointer && m_forwarded_boolean_branch)
		{
			// The helper observed the incoming architectural high word through the
			// pre-call sync. Re-establish recSLTU()'s known-zero high companion
			// before the cold tail rejoins ahead of the producer.
			const size_t high_zero_start = m_code.Size();
			if (!m_code.EmitMovImm8(HOST_TMP5, 0))
				return false;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuResidentForwardedBooleanHighZeroHandlerInstructions +=
				static_cast<u32>((m_code.Size() - high_zero_start) / sizeof(u32));
#endif
		}
		if (m_resident_scheduler_countdown)
		{
			// The helper may change the deadline and clobbers r0/r2/q1. Backing
			// cycle was exact before the call, so rebuild both private scheduler
			// words directly; the next forced translation will save r2 again.
			const size_t stage_start = m_code.Size();
			if (!EmitStageResidentSchedulerCountdown(false))
				return false;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuResidentCycleLowColdReloadInstructions++;
			g_qemuResidentNextEventLowColdReloadInstructions++;
			g_qemuResidentSchedulerCountdownHandlerInstructions += static_cast<u32>(
				(m_code.Size() - stage_start) / sizeof(u32));
#endif
		}
		else if (m_resident_cycle_low)
		{
			// r0 is caller-clobbered and carried the handler address. Rebuild the
			// private scheduler low word before the cold tail rejoins the resident
			// block; the pre-call sync made backing state exact for the helper.
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS,
					static_cast<u16>(CYCLE_OFFSET)))
			{
				return false;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuResidentCycleLowColdReloadInstructions++;
#endif
		}
		if (m_resident_raw_gpr0_qword && tail.rt == 0)
		{
			// q0 is caller-clobbered by the handler ABI. Rebuild the resident raw
			// source before rejoining so a handler-to-RAM page transition cannot
			// feed a clobbered qword into the next native SQ.
			const size_t reload_start = m_code.Size();
			if (!EmitLoadCpuRegsQ128(GprOffset(0), NEON_VALUE, HOST_TMP1))
				return false;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuResidentRawGpr0QwordColdReloadInstructions += static_cast<u32>(
				(m_code.Size() - reload_start) / sizeof(u32));
#endif
		}
		if (m_resident_vtlb_qword_pointer && tail.rt == 0)
		{
			// Direct stores advance r3 by one qword through NEON writeback. A handler
			// bypasses that store, so zero forces the full vTLB path on the next
			// resident entry, including handler-to-direct page transitions.
			const size_t invalidate_start = m_code.Size();
			if (!m_code.EmitMovImm8(HOST_TMP3, 0))
				return false;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuResidentVtlbQwordPointerColdInvalidationInstructions +=
				static_cast<u32>((m_code.Size() - invalidate_start) / sizeof(u32));
#endif
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitCop1WordMemoryColdTail(const Cop1WordMemoryColdTail& tail)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memRead32()/vtlb_memWrite32() plus
		// FPU.cpp::LWC1()/SWC1(). Unaligned FPU word accesses return before
		// memory dispatch in FPU.cpp, while handler pages still dispatch through vTLB.
		if (!m_code.PatchBranch(tail.unaligned_fallback, tail.join_offset, VitaA32::Condition::NE))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.handler_fallback, fallback_target, VitaA32::Condition::MI) ||
			!EmitSyncGprPinsToBacking(&tail.dirty_pins))
		{
			return false;
		}

		if (tail.store)
		{
			if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(tail.rt))) ||
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&memWrite32)))
			{
				return false;
			}
		}
		else if (!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&memRead32)) ||
				 !m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(tail.rt))))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitCop2QwordMemoryColdTail(const Cop2QwordMemoryColdTail& tail)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memRead128()/vtlb_memWrite128()
		// plus VU0.cpp::LQC2()/SQC2(). The handler branch is emitted before
		// the translated host pointer replaces the guest address, so HOST_TMP0
		// still carries the PCSX2 memory address argument on the cold edge.
		constexpr unsigned NEON_VALUE = 0;
		InvalidateGprQCacheForQreg(NEON_VALUE);
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.handler_fallback, fallback_target, VitaA32::Condition::MI) ||
			!EmitVu0ViAddress(HOST_TMP1, VU0_REG_VPU_STAT) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP1, 0) ||
			!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 1, true))
		{
			return false;
		}

		const size_t vu0_idle = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (vu0_idle == static_cast<size_t>(-1) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu0Sync)) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP5, VitaA32::ShiftType::LSL, 0) ||
			!m_code.PatchBranch(vu0_idle, m_code.Size(), VitaA32::Condition::EQ))
		{
			return false;
		}

		if (tail.store)
		{
			if (!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
				!EmitVu0VfAddress(HOST_TMP1, tail.rt) ||
				!m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP1) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP5, VitaA32::ShiftType::LSL, 0) ||
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vtlb_memWrite128)))
			{
				return false;
			}
		}
		else
		{
			if (!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vtlb_memRead128)))
				return false;

			if (tail.rt != 0 &&
				(!EmitVu0VfAddress(HOST_TMP0, tail.rt) ||
				 !m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP0)))
			{
				return false;
			}
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitVu0SyncColdTail(const Vu0SyncColdTail& tail)
	{
		// PCSX2 owners: VU0.cpp::vu0Sync() / _vu0run(). Blocks branch here
		// only when VU0.VI[REG_VPU_STAT].UL bit 0 says macro VU0 is running;
		// idle VU0 stays on the fallthrough path.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.running_branch, fallback_target, VitaA32::Condition::NE))
			return false;

		const bool preserve = tail.preserve_reg < 16;
		if ((preserve &&
			 (!m_code.EmitMovRegShiftImm(tail.save_reg, tail.preserve_reg, VitaA32::ShiftType::LSL, 0))) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu0Sync)) ||
			(preserve &&
			 !m_code.EmitMovRegShiftImm(tail.preserve_reg, tail.save_reg, VitaA32::ShiftType::LSL, 0)))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	void BlockCompiler::CapturePartialStoreValue(PartialMemoryColdTail* tail)
	{
		if (!tail || FindGprPinHost(tail->rt) >= 0)
			return;

		switch (tail->op)
		{
			case PartialMemoryOp::WordStoreLeft:
			case PartialMemoryOp::WordStoreRight:
				tail->rt_low_known = TryGetKnownGprLow(tail->rt, &tail->rt_low);
				break;
			case PartialMemoryOp::DwordStoreLeft:
			case PartialMemoryOp::DwordStoreRight:
				if (tail->rt != 0 && TryGetKnownGpr64(tail->rt, &tail->rt_low, &tail->rt_high))
	{
					tail->rt_low_known = true;
					tail->rt_high_known = true;
	}
				break;
			default:
				break;
		}
	}

	bool BlockCompiler::EmitPartialMemoryColdTail(const PartialMemoryColdTail& tail)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memRead*()/vtlb_memWrite*() plus
		// R5900OpcodeImpl.cpp::LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR().
		// Partial accesses do not raise address errors; handler-backed pages
		// dispatch through PCSX2's vTLB and do the merge in generated A32,
		// while non-handler pages fall through after the native fast path.
		// The dword forms merge byte lanes through the raw backing slots, so
		// deferred pinned words must be resident in backing before the merge.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.handler_fallback, fallback_target, VitaA32::Condition::MI) ||
			!EmitSyncGprPinsToBacking(&tail.dirty_pins))
		{
			return false;
		}

		const auto emit_original_aligned_address = [this](u32 mask) -> bool {
			return m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP5, VitaA32::ShiftType::LSL, 0) &&
				   m_code.EmitBicImm32(HOST_TMP0, HOST_TMP0, mask);
		};

		const auto emit_word_shift_bits = [this](bool left) -> bool {
			if (!m_code.EmitAndImm8(HOST_TMP3, HOST_TMP5, 3) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP3, VitaA32::ShiftType::LSL, 3))
			{
				return false;
			}

			return !left || m_code.EmitRsbImm32(HOST_TMP3, HOST_TMP3, 24);
		};

		const auto emit_word_load = [&](bool left) -> bool {
			if (!emit_original_aligned_address(3) ||
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&memRead32)))
			{
				return false;
			}

			if (tail.rt == 0)
				return true;

			if (!emit_word_shift_bits(left))
				return false;

			if (left)
			{
				return EmitLoadGprLow(tail.rt, HOST_TMP1) &&
					   m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP3, 32) &&
					   m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) &&
					   m_code.EmitAndRegShiftReg(HOST_TMP1, HOST_TMP1, HOST_TMP2,
						   VitaA32::ShiftType::LSR, HOST_TMP4) &&
					   m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP1, HOST_TMP0,
						   VitaA32::ShiftType::LSL, HOST_TMP3) &&
					   EmitStoreGprSignExtended32FromLow(tail.rt, HOST_TMP0);
			}

			return EmitLoadGprLow(tail.rt, HOST_TMP1) &&
				   EmitLoadGprHigh(tail.rt, HOST_TMP2) &&
				   m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP3, 32) &&
				   m_code.EmitMovImm32(HOST_TMP5, 0xffffffffu) &&
				   m_code.EmitAndRegShiftReg(HOST_TMP1, HOST_TMP1, HOST_TMP5,
					   VitaA32::ShiftType::LSL, HOST_TMP4) &&
				   m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP1, HOST_TMP0,
					   VitaA32::ShiftType::LSR, HOST_TMP3) &&
				   m_code.EmitCmpImm32(HOST_TMP3, 0) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::ASR, 31, false,
					   VitaA32::Condition::EQ) &&
				   EmitStoreGpr64(tail.rt, HOST_TMP0, HOST_TMP2);
		};

		const auto emit_word_store = [&](bool left) -> bool {
			if (!emit_original_aligned_address(3) ||
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&memRead32)) ||
				!emit_word_shift_bits(left) ||
				!EmitLoadPartialStoreLowKnownValue(tail.rt, HOST_TMP1, tail.rt_low_known, tail.rt_low) ||
				!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP3, 32) ||
				!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu))
			{
				return false;
			}

			if (!(left ? m_code.EmitAndRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP2,
							   VitaA32::ShiftType::LSL, HOST_TMP4) :
						 m_code.EmitAndRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP2,
							   VitaA32::ShiftType::LSR, HOST_TMP4)) ||
				!(left ? m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP1,
							   VitaA32::ShiftType::LSR, HOST_TMP3) :
						 m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP1,
							   VitaA32::ShiftType::LSL, HOST_TMP3)) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
				!emit_original_aligned_address(3) ||
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&memWrite32)))
			{
				return false;
			}

			return true;
		};

		const auto emit_dword_loop_indices = [this](bool left) -> bool {
			if (!m_code.EmitAndImm8(HOST_TMP0, HOST_TMP5, 7))
				return false;

			if (left)
			{
				return m_code.EmitRsbImm32(HOST_TMP2, HOST_TMP0, 7) &&
					   m_code.EmitAddImm8(HOST_TMP3, HOST_TMP0, 1) &&
					   m_code.EmitMovImm8(HOST_TMP0, 0);
			}

			return m_code.EmitMovImm8(HOST_TMP2, 0) &&
				   m_code.EmitRsbImm32(HOST_TMP3, HOST_TMP0, 8);
		};

		const auto emit_dword_byte_loop = [&](bool store) -> bool {
			const size_t loop_target = m_code.Size();
			if (!(store ? (m_code.EmitLdrbRegShift(HOST_TMP1, HOST_TMP4, HOST_TMP2, VitaA32::ShiftType::LSL, 0) &&
							  m_code.EmitStrbRegShift(HOST_TMP1, HOST_SP, HOST_TMP0, VitaA32::ShiftType::LSL, 0)) :
						  (m_code.EmitLdrbRegShift(HOST_TMP1, HOST_SP, HOST_TMP0, VitaA32::ShiftType::LSL, 0) &&
							  m_code.EmitStrbRegShift(HOST_TMP1, HOST_TMP4, HOST_TMP2, VitaA32::ShiftType::LSL, 0))) ||
				!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0, 1) ||
				!m_code.EmitAddImm8(HOST_TMP2, HOST_TMP2, 1) ||
				!m_code.EmitSubImm8(HOST_TMP3, HOST_TMP3, 1, true))
			{
				return false;
			}

			const size_t loop_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			return loop_branch != static_cast<size_t>(-1) &&
				   m_code.PatchBranch(loop_branch, loop_target, VitaA32::Condition::NE);
		};

		const auto emit_dword_load = [&](bool left) -> bool {
			if (!emit_original_aligned_address(7) ||
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&memRead64)))
			{
				return false;
			}

			if (tail.rt == 0)
				return true;

			return m_code.EmitSubImm8(HOST_SP, HOST_SP, 8) &&
				   m_code.EmitStrdImm8(HOST_TMP0, HOST_TMP1, HOST_SP, 0) &&
				   m_code.EmitAddImm32(HOST_TMP4, HOST_CPU_REGS, static_cast<u32>(GprOffset(tail.rt))) &&
				   emit_dword_loop_indices(left) &&
				   emit_dword_byte_loop(false) &&
				   m_code.EmitAddImm8(HOST_SP, HOST_SP, 8) &&
				   EmitRefreshGprPinFromBacking(tail.rt);
		};

		const auto emit_dword_store = [&](bool left) -> bool {
			const bool known_source = tail.rt != 0 && tail.rt_low_known && tail.rt_high_known;
			const u8 stack_bytes = known_source ? 16 : 8;
			if (!emit_original_aligned_address(7) ||
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&memRead64)) ||
				!m_code.EmitSubImm8(HOST_SP, HOST_SP, stack_bytes) ||
				!m_code.EmitStrdImm8(HOST_TMP0, HOST_TMP1, HOST_SP, 0))
			{
				return false;
			}

			if (known_source)
			{
				if (!m_code.EmitMovImm32(HOST_TMP2, tail.rt_low) ||
					!m_code.EmitMovImm32(HOST_TMP3, tail.rt_high) ||
					!m_code.EmitStrdImm8(HOST_TMP2, HOST_TMP3, HOST_SP, 8) ||
					!m_code.EmitAddImm8(HOST_TMP4, HOST_SP, 8))
	{
					return false;
	}
			}
			else if (!m_code.EmitAddImm32(HOST_TMP4, HOST_CPU_REGS, static_cast<u32>(GprOffset(tail.rt))))
			{
				return false;
			}

			return emit_dword_loop_indices(left) &&
				   emit_dword_byte_loop(true) &&
				   m_code.EmitLdrdImm8(HOST_TMP0, HOST_TMP1, HOST_SP, 0) &&
				   m_code.EmitAddImm8(HOST_SP, HOST_SP, stack_bytes) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 0) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP1, VitaA32::ShiftType::LSL, 0) &&
				   emit_original_aligned_address(7) &&
				   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&memWrite64));
		};

		bool emitted = false;
		switch (tail.op)
		{
			case PartialMemoryOp::WordLoadLeft:
				emitted = emit_word_load(true);
				break;
			case PartialMemoryOp::WordLoadRight:
				emitted = emit_word_load(false);
				break;
			case PartialMemoryOp::WordStoreLeft:
				emitted = emit_word_store(true);
				break;
			case PartialMemoryOp::WordStoreRight:
				emitted = emit_word_store(false);
				break;
			case PartialMemoryOp::DwordLoadLeft:
				emitted = emit_dword_load(true);
				break;
			case PartialMemoryOp::DwordLoadRight:
				emitted = emit_dword_load(false);
				break;
			case PartialMemoryOp::DwordStoreLeft:
				emitted = emit_dword_store(true);
				break;
			case PartialMemoryOp::DwordStoreRight:
				emitted = emit_dword_store(false);
				break;
		}

		// The word-load merges store rt through the deferring seam after the
		// block-exit flush was emitted, so push any tail deferrals back out.
		if (!emitted || !EmitFlushDirtyGprPins())
			return false;

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitDeviceTracePreInstruction(u32 pc)
	{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		return true;
#else
		if (!Pcsx2Trace::IsGsTraceEnabled() && !Pcsx2Trace::IsVuTraceEnabled())
			return true;

		if (Pcsx2Trace::IsGsTraceEnabled())
		{
			if (!m_code.EmitMovImm32(HOST_TMP0, pc) ||
				!m_code.EmitCallAbsolute(
					reinterpret_cast<const void*>(
						static_cast<bool (*)(u32)>(&Pcsx2Trace::RecordGsPreEeInstruction))))
			{
				return false;
			}
		}

		if (Pcsx2Trace::IsVuTraceEnabled())
		{
			if (!m_code.EmitMovImm32(HOST_TMP0, pc) ||
				!m_code.EmitCallAbsolute(
					reinterpret_cast<const void*>(
						static_cast<bool (*)(u32)>(&Pcsx2Trace::RecordVuPreEeInstruction))))
			{
				return false;
			}
		}

		return true;
#endif
	}

	bool BlockCompiler::EmitLoadCpuRegsU64(size_t offset, unsigned host_low, unsigned host_high,
		unsigned address_scratch)
	{
		if (CanUseA32DualTransferPair(host_low, host_high))
		{
			if (offset <= 0xff)
				return m_code.EmitLdrdImm8(host_low, host_high, HOST_CPU_REGS, static_cast<u8>(offset));

			if (EmitCpuRegsAddress(address_scratch, offset))
				return m_code.EmitLdrdImm8(host_low, host_high, address_scratch, 0);
		}

		return m_code.EmitLdrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(offset)) &&
			   m_code.EmitLdrImm12(host_high, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
	}

	bool BlockCompiler::EmitStoreCpuRegsU64(size_t offset, unsigned host_low, unsigned host_high,
		unsigned address_scratch)
	{
		if (CanUseA32DualTransferPair(host_low, host_high))
		{
			if (offset <= 0xff)
				return m_code.EmitStrdImm8(host_low, host_high, HOST_CPU_REGS, static_cast<u8>(offset));

			if (EmitCpuRegsAddress(address_scratch, offset))
				return m_code.EmitStrdImm8(host_low, host_high, address_scratch, 0);
		}

		return m_code.EmitStrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(offset)) &&
			   m_code.EmitStrImm12(host_high, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
	}

	bool BlockCompiler::EmitAddScaledCyclesToCpuLowWord(u32 cycles, unsigned host_low, unsigned scratch,
		size_t* carry_branch)
	{
		if (!carry_branch)
			return false;

		*carry_branch = static_cast<size_t>(-1);
		if (!m_code.EmitLdrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)))
			return false;

		if (!m_code.EmitAddImm32(host_low, host_low, cycles, true))
		{
			if (!m_code.EmitMovImm32(scratch, cycles) ||
				!m_code.EmitAddReg(host_low, host_low, scratch, true))
			{
				return false;
			}
		}

		if (!m_code.EmitStrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)))
			return false;

		*carry_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
		return *carry_branch != static_cast<size_t>(-1);
	}

	bool BlockCompiler::EmitCycleCarryFixup(const size_t* carry_branches, size_t carry_branch_count,
		size_t resume_offset, unsigned scratch)
	{
		if (!carry_branches || carry_branch_count == 0)
			return true;

		bool has_carry_branch = false;
		for (size_t i = 0; i < carry_branch_count; i++)
			has_carry_branch = has_carry_branch || carry_branches[i] != static_cast<size_t>(-1);
		if (!has_carry_branch)
			return true;

		const size_t carry_target = m_code.Size();
		for (size_t i = 0; i < carry_branch_count; i++)
		{
			if (carry_branches[i] != static_cast<size_t>(-1) &&
				!m_code.PatchBranch(carry_branches[i], carry_target, VitaA32::Condition::CS))
			{
				return false;
			}
		}

		constexpr u16 cycle_high_offset = static_cast<u16>(CYCLE_OFFSET + sizeof(u32));
		if (!m_code.EmitLdrImm12(scratch, HOST_CPU_REGS, cycle_high_offset) ||
			!m_code.EmitAddImm8(scratch, scratch, 1) ||
			!m_code.EmitStrImm12(scratch, HOST_CPU_REGS, cycle_high_offset))
		{
			return false;
		}

		const size_t resume_branch = m_code.EmitBranchPlaceholder();
		return resume_branch != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(resume_branch, resume_offset);
	}

	bool BlockCompiler::EmitLoadCpuRegsQ128(size_t offset, unsigned qreg, unsigned address_scratch)
	{
		if (offset == 0)
		{
			// PCSX2 owner: R5900.h::cpuRegistersPack keeps cpuRegs alignas(16)
			// with GPR first, so GPR[0] can use the aligned 128-bit NEON form.
			return m_code.EmitVld1Q32Aligned(qreg, HOST_CPU_REGS);
		}

		const unsigned low_d = qreg * 2;
		const unsigned high_d = low_d + 1;
		if (offset + sizeof(u64) <= 0x3fc && (offset & 0x3u) == 0)
		{
			return m_code.EmitVldrDImm(low_d, HOST_CPU_REGS, static_cast<u16>(offset)) &&
				   m_code.EmitVldrDImm(high_d, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u64)));
		}

		return EmitCpuRegsAddress(address_scratch, offset) &&
			   m_code.EmitVld1Q32Aligned(qreg, address_scratch);
	}

	bool BlockCompiler::EmitStoreCpuRegsQ128(size_t offset, unsigned qreg, unsigned address_scratch)
	{
		if (offset == 0)
		{
			// PCSX2 owner: R5900.h::cpuRegistersPack keeps cpuRegs alignas(16)
			// with GPR first, so GPR[0] can use the aligned 128-bit NEON form.
			return m_code.EmitVst1Q32Aligned(qreg, HOST_CPU_REGS);
		}

		const unsigned low_d = qreg * 2;
		const unsigned high_d = low_d + 1;
		if (offset + sizeof(u64) <= 0x3fc && (offset & 0x3u) == 0)
		{
			return m_code.EmitVstrDImm(low_d, HOST_CPU_REGS, static_cast<u16>(offset)) &&
				   m_code.EmitVstrDImm(high_d, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u64)));
		}

		return EmitCpuRegsAddress(address_scratch, offset) &&
			   m_code.EmitVst1Q32Aligned(qreg, address_scratch);
	}

	bool BlockCompiler::EmitMoveQWordLaneToCore(unsigned host_reg, unsigned qreg, unsigned word)
	{
		return EmitMoveQWordLaneToCore(host_reg, qreg, word, VitaA32::Condition::AL);
	}

	bool BlockCompiler::EmitMoveQWordLaneToCore(unsigned host_reg, unsigned qreg, unsigned word,
		VitaA32::Condition condition)
	{
		if (qreg >= 16 || word >= 4)
			return false;

		return m_code.EmitVmovD32LaneToCore(host_reg, qreg * 2 + word / 2,
			static_cast<u8>(word & 1u), condition);
	}

	bool BlockCompiler::EmitMoveCoreToQWordLane(unsigned qreg, unsigned word, unsigned host_reg)
	{
		return EmitMoveCoreToQWordLane(qreg, word, host_reg, VitaA32::Condition::AL);
	}

	bool BlockCompiler::EmitMoveCoreToQWordLane(unsigned qreg, unsigned word, unsigned host_reg,
		VitaA32::Condition condition)
	{
		if (qreg >= 16 || word >= 4)
			return false;

		return m_code.EmitVmovCoreToD32Lane(qreg * 2 + word / 2,
			static_cast<u8>(word & 1u), host_reg, condition);
	}

	bool BlockCompiler::EmitMoveQWordLane(unsigned dest_qreg, unsigned dest_word,
		unsigned source_qreg, unsigned source_word, unsigned host_scratch)
	{
		if (dest_qreg >= 16 || source_qreg >= 16 || dest_word >= 4 || source_word >= 4)
			return false;
		if (dest_qreg == source_qreg && dest_word == source_word)
			return true;

		// Only q0-q3 have S-register aliases. Preserve their one-instruction lane
		// copy; high-bank copies use the architected D-lane/core transfers.
		if (dest_qreg < 4 && source_qreg < 4)
			return m_code.EmitVmovS(dest_qreg * 4 + dest_word, source_qreg * 4 + source_word);

		return EmitMoveQWordLaneToCore(host_scratch, source_qreg, source_word) &&
			   EmitMoveCoreToQWordLane(dest_qreg, dest_word, host_scratch);
	}

	bool BlockCompiler::EmitLoadQWordLane(unsigned qreg, unsigned word, unsigned address_reg,
		u16 offset, unsigned host_scratch)
	{
		if (qreg >= 16 || word >= 4)
			return false;
		if (qreg < 4)
			return m_code.EmitVldrSImm(qreg * 4 + word, address_reg, offset);

		return m_code.EmitLdrImm12(host_scratch, address_reg, offset) &&
			   EmitMoveCoreToQWordLane(qreg, word, host_scratch);
	}

	bool BlockCompiler::EmitStoreQWordLane(unsigned qreg, unsigned word, unsigned address_reg,
		u16 offset, unsigned host_scratch)
	{
		if (qreg >= 16 || word >= 4)
			return false;
		if (offset == 0)
		{
			return m_code.EmitVst1D32Lane(qreg * 2 + word / 2,
				static_cast<u8>(word & 1u), address_reg);
		}
		if (qreg < 4)
			return m_code.EmitVstrSImm(qreg * 4 + word, address_reg, offset);

		return EmitMoveQWordLaneToCore(host_scratch, qreg, word) &&
			   m_code.EmitStrImm12(host_scratch, address_reg, offset);
	}

	bool BlockCompiler::EmitCop1ExponentMask(unsigned host_reg)
	{
		if (!m_cop1_exponent_mask_available)
			return m_code.EmitMovImm32(host_reg, FPU_FLOAT_EXPONENT_MASK);

		if (host_reg == HOST_COP1_EXPONENT_MASK)
			return true;

		return m_code.EmitMovRegShiftImm(host_reg, HOST_COP1_EXPONENT_MASK, VitaA32::ShiftType::LSL, 0);
	}

	bool BlockCompiler::EmitAndCop1ExponentMask(unsigned rd, unsigned rn, unsigned scratch)
	{
		if (m_cop1_exponent_mask_available)
			return m_code.EmitAndReg(rd, rn, HOST_COP1_EXPONENT_MASK);

		return EmitAndImm32OrReg(rd, rn, FPU_FLOAT_EXPONENT_MASK, scratch);
	}

	bool BlockCompiler::EmitAndCop1FractionMask(unsigned rd, unsigned rn)
	{
		// PCSX2 owner: FPU.cpp fast paths inspect IEEE-754 fraction bits as
		// word & 0x007fffff; A32 UBFX extracts the same bit range in one insn.
		return m_code.EmitUbfx(rd, rn, 0, 23);
	}

	bool BlockCompiler::EmitAddScaledCyclesToCpu(u32 cycles)
	{
		if (!EmitLoadCpuRegsU64(CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP2))
			return false;

		if (!m_code.EmitAddImm32(HOST_TMP0, HOST_TMP0, cycles, true))
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, cycles) ||
				!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true))
			{
				return false;
			}
		}

		return m_code.EmitAdcImm8(HOST_TMP1, HOST_TMP1, 0) &&
			   EmitStoreCpuRegsU64(CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP2);
	}

	bool BlockCompiler::EmitEffectiveAddress(u32 op, unsigned host_reg)
	{
		const unsigned rs = RS(op);
		const s32 imm = static_cast<s32>(IMM_S(op));

		// PCSX2 owner: R5900OpcodeImpl.cpp memory ops form
		// cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_ before vtlb_memRead/Write.
		if (rs == 0)
			return m_code.EmitMovImm32(host_reg, static_cast<u32>(imm));

		u32 known_address = 0;
		if (FindGprPinHost(rs) < 0 && TryGetKnownEffectiveAddress(op, &known_address))
		{
			// Known VTLB-fast paths consume constants earlier; this catches the
			// remaining handler/unaligned tails and avoids a cpuRegs load-use.
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprConstEffectiveAddresses++;
#endif
			return m_code.EmitMovImm32(host_reg, known_address);
		}

		// Callers mutate the address register, so fold a pinned base into the
		// displacement add instead of returning the pin itself.
		unsigned base_reg;
		if (!EmitGprLowOperand(rs, host_reg, &base_reg))
			return false;

		if (imm == 0)
		{
			return base_reg == host_reg ||
				   m_code.EmitMovRegShiftImm(host_reg, base_reg, VitaA32::ShiftType::LSL, 0);
		}

		if (imm > 0 && m_code.EmitAddImm32(host_reg, base_reg, static_cast<u32>(imm)))
			return true;

		if (imm < 0 && m_code.EmitSubImm32(host_reg, base_reg, static_cast<u32>(-imm)))
			return true;

		const unsigned scratch_reg = (host_reg == HOST_TMP2) ? HOST_TMP0 : HOST_TMP2;
		return m_code.EmitMovImm32(scratch_reg, static_cast<u32>(imm)) &&
			   m_code.EmitAddReg(host_reg, base_reg, scratch_reg);
	}

	bool BlockCompiler::EmitCpuRegsAddress(unsigned host_reg, size_t offset)
	{
		// PCSX2 owner: R5900.h::cpuRegisters keeps GPR/HI/LO as contiguous
		// 16-byte slots, so these guest-state addresses can use A32's
		// modified-immediate ADD whenever the struct offset encodes directly.
		if (offset <= 0xffffffffu && m_code.EmitAddImm32(host_reg, HOST_CPU_REGS, static_cast<u32>(offset)))
			return true;

		return m_code.EmitMovImm32(host_reg, static_cast<u32>(offset)) &&
			   m_code.EmitAddReg(host_reg, HOST_CPU_REGS, host_reg);
	}

	bool BlockCompiler::EmitLoadRawGpr0KnownZeroFlag(unsigned host_reg)
	{
		return m_code.EmitMovImm32(host_reg, static_cast<u32>(reinterpret_cast<uptr>(&s_raw_gpr0_known_zero))) &&
			   m_code.EmitLdrImm12(host_reg, host_reg, 0);
	}

	bool BlockCompiler::EmitRefreshRawGpr0KnownZeroFromLow64(unsigned low_reg, unsigned high_reg)
	{
		const size_t high64_offset = GprOffset(0) + 2 * sizeof(u32);
		return m_code.EmitOrrReg(HOST_TMP2, low_reg, high_reg) &&
			   m_code.EmitLdrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(high64_offset)) &&
			   m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP3) &&
			   m_code.EmitLdrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(high64_offset + sizeof(u32))) &&
			   m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP3, true) &&
			   m_code.EmitMovImm8(HOST_TMP2, 0) &&
			   m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::EQ) &&
			   m_code.EmitMovImm32(HOST_TMP3, static_cast<u32>(reinterpret_cast<uptr>(&s_raw_gpr0_known_zero))) &&
			   m_code.EmitStrImm12(HOST_TMP2, HOST_TMP3, 0);
	}

	bool BlockCompiler::EmitVu0SyncIfRunning(unsigned preserve_reg, unsigned save_reg)
	{
		// PCSX2 owners: VU0.cpp::vu0Sync() / _vu0run() and
		// x86/microVU_Macro.inl::mVUSyncVU0(). _vu0run() returns before any
		// side effect when VU0.VI[REG_VPU_STAT].UL bit 0 is clear. Keep that
		// idle case as fallthrough and branch only the running case to a cold
		// helper tail.
		if (!EmitVu0ViAddress(HOST_TMP1, VU0_REG_VPU_STAT) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP1, 0) ||
			!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 1, true))
		{
			return false;
		}

		const size_t vu0_running = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (vu0_running == static_cast<size_t>(-1))
			return false;

		m_vu0_sync_cold_tails.push_back({
			vu0_running,
			m_code.Size(),
			preserve_reg,
			save_reg,
		});
		return true;
	}

	bool BlockCompiler::EmitVu0RegisterAddress(unsigned host_reg, size_t offset)
	{
		// PCSX2 owners: VU0.cpp and VU.h keep VU0 VF/VI state in one singleton.
		// Repeated COP2/VU0 memory blocks can address it from a resident base.
		if (m_vu0_base_available)
		{
			if (offset == 0)
			{
				if (host_reg == HOST_VU0_BASE)
					return true;

				return m_code.EmitMovRegShiftImm(host_reg, HOST_VU0_BASE, VitaA32::ShiftType::LSL, 0);
			}

			if (m_code.EmitAddImm32(host_reg, HOST_VU0_BASE, static_cast<u32>(offset)))
				return true;
		}

		return m_code.EmitMovImm32(host_reg,
			static_cast<u32>(reinterpret_cast<uptr>(&VU0)) + static_cast<u32>(offset));
	}

	bool BlockCompiler::EmitVu0ClipflagAddress(unsigned host_reg)
	{
		// PCSX2 owner: VU.h::VURegs keeps clipflag in the VU0 singleton beside
		// the VI mirror. Repeated VCLIP/CTC2 blocks should reuse the resident
		// VU0 base instead of re-materializing this absolute side-field address.
		if (m_vu0_base_available &&
			m_code.EmitAddImm32(host_reg, HOST_VU0_BASE, static_cast<u32>(VU0_CLIPFLAG_OFFSET)))
		{
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuVu0ClipflagBaseAddressFastPaths++;
#endif
			return true;
		}

		return m_code.EmitMovImm32(host_reg,
			static_cast<u32>(reinterpret_cast<uptr>(&VU0)) + static_cast<u32>(VU0_CLIPFLAG_OFFSET));
	}

	bool BlockCompiler::EmitVu0Vf0ConstantQ(unsigned qreg, unsigned host_scratch)
	{
		if (qreg >= 16)
			return false;

		// PCSX2 owners: VUmicroMem.cpp initializes VF0 to 0,0,0,1 and
		// VU0.cpp::QMTC2() rejects writes to VF0, so readers can synthesize it.
		return m_code.EmitVeorQ(qreg, qreg, qreg) &&
			   m_code.EmitMovImm32(host_scratch, 0x3f800000u) &&
			   EmitMoveCoreToQWordLane(qreg, 3, host_scratch);
	}

	bool BlockCompiler::EmitVu0VfAddress(unsigned host_reg, unsigned vf_reg)
	{
		return EmitVu0RegisterAddress(host_reg, VU0_VF_OFFSET + vf_reg * VU0_VF_STRIDE);
	}

	bool BlockCompiler::EmitVu0ViAddress(unsigned host_reg, unsigned vi_reg)
	{
		return EmitVu0RegisterAddress(host_reg, VU0_VI_OFFSET + vi_reg * VU0_VI_STRIDE);
	}

	bool BlockCompiler::EmitAlignQwordAddress(unsigned host_reg, unsigned)
	{
		return m_code.EmitBicImm32(host_reg, host_reg, 0x0f);
	}

	bool BlockCompiler::EmitVtlbNonHandlerHostAddress(unsigned host_reg, unsigned vmap_reg,
		unsigned scratch_reg, size_t* handler_fallback_branch, GprPinDirtyMasks* dirty_pins)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memRead*() / vtlb_memWrite*().
		// Fast path only mirrors the non-handler VTLB case. Handler-backed
		// cache/MMIO/unmapped pages branch to the existing PCSX2 helper path.
		constexpr u8 VTLB_VIRTUAL_ENTRY_SHIFT = 2;
		static_assert((sizeof(vtlb_private::VTLBVirtual) >> VTLB_VIRTUAL_ENTRY_SHIFT) == 1);
		if (dirty_pins)
			*dirty_pins = CurrentGprPinDirtyMasks();

		if (m_vtlb_registers_available)
		{
			if (!m_code.EmitMovRegShiftImm(scratch_reg, host_reg, VitaA32::ShiftType::LSR,
					vtlb_private::VTLB_PAGE_BITS) ||
				!m_code.EmitLdrRegShift(vmap_reg, HOST_VTLB_VMAP, scratch_reg, VitaA32::ShiftType::LSL,
					VTLB_VIRTUAL_ENTRY_SHIFT) ||
				!m_code.EmitAddReg(vmap_reg, vmap_reg, host_reg, true))
			{
				return false;
			}

			*handler_fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::MI);
			if (*handler_fallback_branch == static_cast<size_t>(-1))
				return false;

			return m_code.EmitAddReg(host_reg, vmap_reg, HOST_VTLB_HOST_MEMORY_BASE);
		}

		if (!m_code.EmitMovImm32(vmap_reg,
				static_cast<u32>(reinterpret_cast<uptr>(&vtlb_private::vtlbdata.vmap))) ||
			!m_code.EmitLdrImm12(vmap_reg, vmap_reg, 0) ||
			!m_code.EmitMovRegShiftImm(scratch_reg, host_reg, VitaA32::ShiftType::LSR,
				vtlb_private::VTLB_PAGE_BITS) ||
			!m_code.EmitLdrRegShift(vmap_reg, vmap_reg, scratch_reg, VitaA32::ShiftType::LSL,
				VTLB_VIRTUAL_ENTRY_SHIFT) ||
			!m_code.EmitAddReg(vmap_reg, vmap_reg, host_reg, true))
		{
			return false;
		}

		*handler_fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::MI);
		if (*handler_fallback_branch == static_cast<size_t>(-1))
			return false;

		return m_code.EmitMovImm32(scratch_reg,
				   static_cast<u32>(reinterpret_cast<uptr>(&vtlb_private::vtlbdata.host_memory_base))) &&
			   m_code.EmitLdrImm12(scratch_reg, scratch_reg, 0) &&
			   m_code.EmitAddReg(host_reg, vmap_reg, scratch_reg);
	}

	bool BlockCompiler::EmitVtlbNonHandlerHostAddress128(unsigned host_reg, unsigned vmap_reg,
		unsigned scratch_reg, size_t* handler_fallback_branch, GprPinDirtyMasks* dirty_pins)
	{
		return EmitVtlbNonHandlerHostAddress(
			host_reg, vmap_reg, scratch_reg, handler_fallback_branch, dirty_pins);
	}

	bool BlockCompiler::EmitGprLowOperand(unsigned guest_reg, unsigned fallback_host, unsigned* operand_host)
	{
		// Pinned guest registers are handed out directly as read-only A32 source
		// operands; the caller must consume the operand before any code that can
		// write that guest register and must never modify the returned register.
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			*operand_host = static_cast<unsigned>(pin_host);
			return true;
		}

		*operand_host = fallback_host;
		return EmitLoadGprLow(guest_reg, fallback_host);
	}

	bool BlockCompiler::EmitLoadGprLowKnownValue(unsigned guest_reg, unsigned host_reg,
		bool value_known, u32 value)
	{
		// PCSX2 x86 recStore() relies on host register allocation for RT store
		// data. Vita's scalar store path has no cross-op x86 allocator, so use
		// block-local constants to avoid a Cortex-A9 cpuRegs load when no pin
		// already holds the value.
		if (guest_reg != 0 && FindGprPinHost(guest_reg) < 0 && value_known)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprConstStoreValueFastPaths++;
#endif
			return m_code.EmitMovImm32(host_reg, value);
		}

		return EmitLoadGprLow(guest_reg, host_reg);
	}

	bool BlockCompiler::EmitLoadGprLowValue(unsigned guest_reg, unsigned host_reg)
	{
		u32 value = 0;
		const bool value_known = TryGetKnownGprLow(guest_reg, &value);
		return EmitLoadGprLowKnownValue(guest_reg, host_reg, value_known, value);
	}

	bool BlockCompiler::EmitGprLowValueOperand(unsigned guest_reg, unsigned fallback_host,
		unsigned* operand_host)
	{
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			*operand_host = static_cast<unsigned>(pin_host);
			return true;
		}

		*operand_host = fallback_host;
		return EmitLoadGprLowValue(guest_reg, fallback_host);
	}

	bool BlockCompiler::EmitLoadPartialStoreLowValue(unsigned guest_reg, unsigned host_reg)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		if (FindGprPinHost(guest_reg) >= 0 || (guest_reg != 0 && TryGetKnownGprLow(guest_reg, nullptr)))
			g_qemuGprPartialStoreValueFastPaths++;
#endif
		return EmitLoadGprLowValue(guest_reg, host_reg);
	}

	bool BlockCompiler::EmitLoadPartialStoreLowKnownValue(unsigned guest_reg, unsigned host_reg,
		bool value_known, u32 value)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		if (FindGprPinHost(guest_reg) >= 0 || (guest_reg != 0 && value_known))
			g_qemuGprPartialStoreValueFastPaths++;
#endif
		return EmitLoadGprLowKnownValue(guest_reg, host_reg, value_known, value);
	}

	bool BlockCompiler::EmitLoadPartialDwordStoreByteValue(unsigned guest_reg, unsigned source_byte,
		unsigned host_reg, bool value_known, u32 low, u32 high)
	{
		if (guest_reg != 0 && value_known)
		{
			const u32 word = (source_byte < 4) ? low : high;
			const u32 byte = (word >> ((source_byte & 3u) * 8u)) & 0xffu;
			return m_code.EmitMovImm8(host_reg, static_cast<u8>(byte));
		}

		return m_code.EmitLdrbImm12(host_reg, HOST_CPU_REGS,
			static_cast<u16>(GprOffset(guest_reg) + source_byte));
	}

	bool BlockCompiler::EmitLoadGprWord(unsigned guest_reg, unsigned word, unsigned host_reg)
	{
		if (word == 0)
			return EmitLoadGprLow(guest_reg, host_reg);
		if (word == 1)
			return EmitLoadGprHigh(guest_reg, host_reg);

		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_reg, 0);

		bool qcache_loaded = false;
		if (!TryEmitLoadGprWordFromQCache(guest_reg, word, host_reg, &qcache_loaded))
			return false;
		if (qcache_loaded)
			return true;

		return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS,
			static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32)));
	}

	bool BlockCompiler::EmitGprWordOperand(unsigned guest_reg, unsigned word, unsigned fallback_host,
		unsigned* operand_host)
	{
		if (word == 0)
			return EmitGprLowOperand(guest_reg, fallback_host, operand_host);
		if (word == 1)
		{
			const int high_pin_host = FindGprPinHighHost(guest_reg);
			if (high_pin_host >= 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprPinnedHighWordHits++;
#endif
				*operand_host = static_cast<unsigned>(high_pin_host);
				return true;
			}

			*operand_host = fallback_host;
			return EmitLoadGprHigh(guest_reg, fallback_host);
		}

		*operand_host = fallback_host;
		if (guest_reg == 0)
			return m_code.EmitMovImm8(fallback_host, 0);

		bool qcache_loaded = false;
		if (!TryEmitLoadGprWordFromQCache(guest_reg, word, fallback_host, &qcache_loaded))
			return false;
		if (qcache_loaded)
			return true;

		return m_code.EmitLdrImm12(fallback_host, HOST_CPU_REGS,
			static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32)));
	}

	bool BlockCompiler::EmitGpr64ReadOperands(unsigned guest_reg, unsigned fallback_low,
		unsigned fallback_high, unsigned* low_operand_host, unsigned* high_operand_host)
	{
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			*low_operand_host = static_cast<unsigned>(pin_host);
			const int high_pin_host = FindGprPinHighHost(guest_reg);
			if (high_pin_host >= 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprPinHighReadOperands++;
#endif
				*high_operand_host = static_cast<unsigned>(high_pin_host);
				return true;
			}

			*high_operand_host = fallback_high;
			return EmitLoadGprHigh(guest_reg, fallback_high);
		}

		*low_operand_host = fallback_low;
		*high_operand_host = fallback_high;
		return EmitLoadGpr64(guest_reg, fallback_low, fallback_high);
	}

	bool BlockCompiler::EmitLoadGpr64KnownValue(unsigned guest_reg, unsigned host_low, unsigned host_high,
		bool value_known, u32 low, u32 high)
	{
		if (guest_reg != 0 && FindGprPinHost(guest_reg) < 0 && value_known)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprConstStoreValueFastPaths++;
#endif
			return m_code.EmitMovImm32(host_low, low) &&
				   m_code.EmitMovImm32(host_high, high);
		}

		return EmitLoadGpr64(guest_reg, host_low, host_high);
	}

	bool BlockCompiler::EmitLoadGpr64Value(unsigned guest_reg, unsigned host_low, unsigned host_high)
	{
		u32 low = 0;
		u32 high = 0;
		const bool value_known = TryGetKnownGpr64(guest_reg, &low, &high);
		return EmitLoadGpr64KnownValue(guest_reg, host_low, host_high, value_known, low, high);
	}

	bool BlockCompiler::EmitGpr64ValueReadOperands(unsigned guest_reg, unsigned fallback_low,
		unsigned fallback_high, unsigned* low_operand_host, unsigned* high_operand_host)
	{
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			*low_operand_host = static_cast<unsigned>(pin_host);
			const int high_pin_host = FindGprPinHighHost(guest_reg);
			if (high_pin_host >= 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprPinHighReadOperands++;
#endif
				*high_operand_host = static_cast<unsigned>(high_pin_host);
				return true;
			}

			*high_operand_host = fallback_high;
			return EmitLoadGprHigh(guest_reg, fallback_high);
		}

		*low_operand_host = fallback_low;
		*high_operand_host = fallback_high;
		return EmitLoadGpr64Value(guest_reg, fallback_low, fallback_high);
	}

	bool BlockCompiler::EmitLoadGprLow(unsigned guest_reg, unsigned host_reg)
	{
		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_reg, 0);

		// Pinned registers carry the current in-block value, so a 1-cycle move
		// replaces the Cortex-A9 load-use stall.
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			return m_code.EmitMovRegShiftImm(host_reg, static_cast<unsigned>(pin_host),
				VitaA32::ShiftType::LSL, 0);
		}

		bool qcache_loaded = false;
		if (!TryEmitLoadGprWordFromQCache(guest_reg, 0, host_reg, &qcache_loaded))
			return false;
		if (qcache_loaded)
			return true;

		return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(GprOffset(guest_reg)));
	}

	bool BlockCompiler::EmitLoadGprLowRawZero(unsigned guest_reg, unsigned host_reg)
	{
		if (guest_reg != 0)
		{
			u32 value = 0;
			const bool value_known = TryGetKnownGprLow(guest_reg, &value);
#if defined(VITASX2_QEMU_VALIDATION)
			if (FindGprPinHost(guest_reg) < 0 && value_known)
				g_qemuCop2ControlKnownSourceFastPaths++;
#endif
			return EmitLoadGprLowKnownValue(guest_reg, host_reg, value_known, value);
		}

		// PCSX2 VU0.cpp::CTC2() reads cpuRegs.GPR.r[_Rt_].UL[0] directly, so
		// rt=$zero observes the raw backing slot instead of architectural zero.
		return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(GprOffset(0)));
	}

	bool BlockCompiler::EmitRefreshGprPinFromBacking(unsigned guest_reg)
	{
		if (guest_reg == 0)
			return true;

		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host < 0)
			return true;

		const size_t offset = GprOffset(guest_reg);
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0 && offset <= 0xff &&
			CanUseA32DualTransferPair(static_cast<unsigned>(pin_host), static_cast<unsigned>(high_pin_host)))
		{
			return m_code.EmitLdrdImm8(static_cast<unsigned>(pin_host), static_cast<unsigned>(high_pin_host),
				HOST_CPU_REGS, static_cast<u8>(offset));
		}

		if (!m_code.EmitLdrImm12(static_cast<unsigned>(pin_host), HOST_CPU_REGS,
			static_cast<u16>(offset)))
		{
			return false;
		}

		return high_pin_host < 0 ||
			   m_code.EmitLdrImm12(static_cast<unsigned>(high_pin_host), HOST_CPU_REGS,
				   static_cast<u16>(offset + sizeof(u32)));
	}

	bool BlockCompiler::TryEmitLoadGprWordFromQCache(unsigned guest_reg, unsigned word,
		unsigned host_reg, bool* emitted)
	{
		*emitted = false;
		if (word >= 4)
			return true;

		const int cached_qreg = FindGprQCache(guest_reg);
		if (cached_qreg < 0)
			return true;

		*emitted = true;
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprQCacheWordHits++;
#endif
		return EmitMoveQWordLaneToCore(host_reg, static_cast<unsigned>(cached_qreg), word);
	}

	bool BlockCompiler::EmitLoadGprHigh(unsigned guest_reg, unsigned host_reg)
	{
		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_reg, 0);

		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprPinnedHighWordHits++;
#endif
			return m_code.EmitMovRegShiftImm(host_reg, static_cast<unsigned>(high_pin_host),
				VitaA32::ShiftType::LSL, 0);
		}

		u32 high_value = 0;
		if (TryGetKnownGpr64(guest_reg, nullptr, &high_value))
		{
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprConstHighWordLoadFastPaths++;
#endif
			return m_code.EmitMovImm32(host_reg, high_value);
		}

		bool qcache_loaded = false;
		if (!TryEmitLoadGprWordFromQCache(guest_reg, 1, host_reg, &qcache_loaded))
			return false;
		if (qcache_loaded)
			return true;

#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprHighBackingLoadInstructions++;
#endif
		return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(GprOffset(guest_reg) + sizeof(u32)));
	}

	bool BlockCompiler::EmitLoadGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high)
	{
		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_low, 0) &&
				   m_code.EmitMovImm8(host_high, 0);

		const size_t offset = GprOffset(guest_reg);
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			return m_code.EmitMovRegShiftImm(host_low, static_cast<unsigned>(pin_host),
					   VitaA32::ShiftType::LSL, 0) &&
				   EmitLoadGprHigh(guest_reg, host_high);
		}

		bool qcache_loaded = false;
		if (!TryEmitLoadGprWordFromQCache(guest_reg, 0, host_low, &qcache_loaded))
			return false;
		if (qcache_loaded)
		{
			bool high_qcache_loaded = false;
			return TryEmitLoadGprWordFromQCache(guest_reg, 1, host_high, &high_qcache_loaded) &&
				   high_qcache_loaded;
		}

		if (offset <= 0xff && CanUseA32DualTransferPair(host_low, host_high))
		{
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGpr64BackingLoadInstructions++;
#endif
			return m_code.EmitLdrdImm8(host_low, host_high, HOST_CPU_REGS, static_cast<u8>(offset));
		}

#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGpr64BackingLoadInstructions += 2;
#endif
		return m_code.EmitLdrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(offset)) &&
		       m_code.EmitLdrImm12(host_high, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
	}

	bool BlockCompiler::EmitLoadGprQ128(unsigned guest_reg, unsigned qreg, unsigned address_scratch)
	{
		const int cached_qreg = FindGprQCache(guest_reg);
		if (cached_qreg >= 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheHits++;
#endif
			const unsigned source_qreg = static_cast<unsigned>(cached_qreg);
			if (source_qreg == qreg)
				return true;

			InvalidateGprQCacheForQreg(qreg);
			return m_code.EmitVorrQ(qreg, source_qreg, source_qreg);
		}

		InvalidateGprQCacheForQreg(qreg);
		if (guest_reg == 0)
			return m_code.EmitVeorQ(qreg, qreg, qreg);

		// Full-qword reads consume the raw backing slot, so deferred pinned
		// low/high words must land in backing first.
		if (!EmitFlushDirtyGprPinsForGuest(guest_reg))
			return false;

#if defined(VITASX2_QEMU_VALIDATION)
			if (m_gpr_q_cache_enabled)
			{
				g_qemuGprQCacheMisses++;
				CountGprQCacheMissForOpcode(m_current_opcode, guest_reg,
					GprQCacheGuestDefinedBeforeCurrentInstruction(guest_reg),
					GprQCacheGuestEntryQwordReadCount(guest_reg));
			}
#endif
		if (!EmitLoadCpuRegsQ128(GprOffset(guest_reg), qreg, address_scratch))
			return false;

		MarkGprQCache(guest_reg, qreg);
		return true;
	}

	bool BlockCompiler::ShouldLoadGprQ128SingleUseEntry(unsigned guest_reg) const
	{
		return guest_reg != 0 && FindGprQCache(guest_reg) < 0 &&
		       !GprQCacheGuestHasFutureQfsrvSourceReadBeforeWrite(guest_reg) &&
		       !GprQCacheGuestDefinedBeforeCurrentInstruction(guest_reg) &&
		       GprQCacheGuestEntryQwordReadCount(guest_reg) == 1;
	}

	bool BlockCompiler::EmitLoadGprQ128SingleUseEntry(unsigned guest_reg, unsigned qreg,
		unsigned address_scratch)
	{
		InvalidateGprQCacheForQreg(qreg);
		if (guest_reg == 0)
			return m_code.EmitVeorQ(qreg, qreg, qreg);

		if (!EmitFlushDirtyGprPinsForGuest(guest_reg))
			return false;

#if defined(VITASX2_QEMU_VALIDATION)
		if (m_gpr_q_cache_enabled)
			g_qemuGprQCacheSingleUseEntryQwordLoads++;
#endif
		return EmitLoadCpuRegsQ128(GprOffset(guest_reg), qreg, address_scratch);
	}

	bool BlockCompiler::EmitStorePc(u32 pc)
	{
		return m_code.EmitMovImm32(HOST_TMP0, pc) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(PC_OFFSET));
	}

	bool BlockCompiler::EmitStorePcFromHostReg(unsigned host_reg)
	{
		return m_code.EmitStrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(PC_OFFSET));
	}

	bool BlockCompiler::EmitStoreBranchPc(u32 target_pc, u32 fallthrough_pc)
	{
		if (target_pc == fallthrough_pc)
			return EmitStorePc(target_pc);

		// PCSX2 owners: Interpreter.cpp::_doBranch_shared() selects the taken
		// target while x86/ix86-32/iR5900.cpp::SetBranchImm() writes that selected
		// PC before iBranchTest(). Select the same value with predicated A32 moves
		// and issue one store, rather than branching over a second full PC store.
		const u16 fallthrough_upper = static_cast<u16>(fallthrough_pc >> 16);
		if ((target_pc >> 16) == fallthrough_upper)
		{
			// Direct branch targets almost always share the fallthrough PC's upper
			// half. Select the low MOVW, then materialize their common MOVT once.
			if (!m_code.EmitMovw(HOST_TMP0, static_cast<u16>(fallthrough_pc)) ||
				!m_code.EmitCmpImm32(m_branch_flag_host, 0) ||
				!m_code.EmitMovw(HOST_TMP0, static_cast<u16>(target_pc), VitaA32::Condition::NE) ||
				(fallthrough_upper != 0 && !m_code.EmitMovt(HOST_TMP0, fallthrough_upper)))
			{
				return false;
			}
		}
		else if (!m_code.EmitMovImm32(HOST_TMP0, fallthrough_pc) ||
			!m_code.EmitCmpImm32(m_branch_flag_host, 0) ||
			!m_code.EmitMovImm32(HOST_TMP0, target_pc, VitaA32::Condition::NE))
		{
			return false;
		}

		return m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(PC_OFFSET));
	}

	bool BlockCompiler::EmitStoreGprZero64(unsigned guest_reg)
	{
		if (guest_reg == 0)
			return true;

		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0 && !m_code.EmitMovImm8(static_cast<unsigned>(pin_host), 0))
			return false;
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0 && !m_code.EmitMovImm8(static_cast<unsigned>(high_pin_host), 0))
			return false;

		const bool defer_low = TryDeferGprPinLowStore(guest_reg);
		const bool defer_high = TryDeferGprPinHighStore(guest_reg);
		const size_t offset = GprOffset(guest_reg);
		bool stored = false;
		if (!defer_low && !defer_high && offset <= 0xff && CanUseA32DualTransferPair(HOST_TMP0, HOST_TMP1))
		{
			stored = m_code.EmitMovImm8(HOST_TMP0, 0) &&
					 m_code.EmitMovImm8(HOST_TMP1, 0) &&
					 m_code.EmitStrdImm8(HOST_TMP0, HOST_TMP1, HOST_CPU_REGS, static_cast<u8>(offset));
		}
		else
		{
			stored = (defer_low && defer_high) || m_code.EmitMovImm8(HOST_TMP0, 0);
			if (stored && !defer_low)
				stored = m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(offset));
			if (stored && !defer_high)
				stored = m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
		}

		if (!stored)
			return false;

		InvalidateGprQCacheForGuest(guest_reg);
		return true;
	}

	bool BlockCompiler::EmitStoreGprQ128(unsigned guest_reg, unsigned qreg, unsigned address_scratch)
	{
		if (guest_reg == 0)
			return true;

		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0 &&
			!EmitMoveQWordLaneToCore(static_cast<unsigned>(pin_host), qreg, 0))
			return false;
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0 &&
			!EmitMoveQWordLaneToCore(static_cast<unsigned>(high_pin_host), qreg, 1))
		{
			return false;
		}

		if (!EmitStoreCpuRegsQ128(GprOffset(guest_reg), qreg, address_scratch))
			return false;

		MarkGprQCache(guest_reg, qreg);
		return true;
	}

	bool BlockCompiler::EmitStoreGprQ128ToAddress(unsigned guest_reg, unsigned qreg, unsigned address_reg)
	{
		if (guest_reg == 0)
			return true;

		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0 &&
			!EmitMoveQWordLaneToCore(static_cast<unsigned>(pin_host), qreg, 0))
			return false;
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0 &&
			!EmitMoveQWordLaneToCore(static_cast<unsigned>(high_pin_host), qreg, 1))
		{
			return false;
		}

		if (!m_code.EmitVst1Q32Aligned(qreg, address_reg))
			return false;

		MarkGprQCache(guest_reg, qreg);
		return true;
	}

	bool BlockCompiler::EmitStoreGprDwordPair(unsigned guest_reg, unsigned low_d, unsigned high_d)
	{
		if (guest_reg == 0)
			return true;

		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0 &&
			!m_code.EmitVmovD32LaneToCore(static_cast<unsigned>(pin_host), low_d, 0))
			return false;
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0 &&
			!m_code.EmitVmovD32LaneToCore(static_cast<unsigned>(high_pin_host), low_d, 1))
			return false;

		if (!m_code.EmitVstrDImm(low_d, HOST_CPU_REGS, static_cast<u16>(GprOffset(guest_reg))) ||
			!m_code.EmitVstrDImm(high_d, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(guest_reg) + sizeof(u64))))
		{
			return false;
		}

		InvalidateGprQCacheForGuest(guest_reg);
		InvalidateGprQCacheForQreg(low_d / 2);
		InvalidateGprQCacheForQreg(high_d / 2);
		return true;
	}

	bool BlockCompiler::EmitStoreGprWord(unsigned guest_reg, unsigned word, unsigned host_reg)
	{
		if (guest_reg == 0)
			return true;

		bool deferred = false;
		if (word == 0)
		{
			const int pin_host = FindGprPinHost(guest_reg);
			if (pin_host >= 0 && static_cast<unsigned>(pin_host) != host_reg &&
				!m_code.EmitMovRegShiftImm(static_cast<unsigned>(pin_host), host_reg,
					VitaA32::ShiftType::LSL, 0))
			{
				return false;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			if (pin_host >= 0 && static_cast<unsigned>(pin_host) == host_reg)
				g_qemuGprPinSelfStoresElided++;
#endif

			deferred = TryDeferGprPinLowStore(guest_reg);
		}
		else if (word == 1)
		{
			const int high_pin_host = FindGprPinHighHost(guest_reg);
			if (high_pin_host >= 0 && static_cast<unsigned>(high_pin_host) != host_reg &&
				!m_code.EmitMovRegShiftImm(static_cast<unsigned>(high_pin_host), host_reg,
					VitaA32::ShiftType::LSL, 0))
			{
				return false;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			if (high_pin_host >= 0 && static_cast<unsigned>(high_pin_host) == host_reg)
				g_qemuGprPinSelfStoresElided++;
#endif

			deferred = TryDeferGprPinHighStore(guest_reg);
		}

		if (!deferred &&
			!m_code.EmitStrImm12(host_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32))))
		{
			return false;
		}

		InvalidateGprQCacheForGuest(guest_reg);
		return true;
	}

	bool BlockCompiler::EmitStoreGprLowPreserveHigh(unsigned guest_reg, unsigned host_low)
	{
		if (guest_reg == 0)
			return true;

		// PCSX2 VU0.cpp::CFC2(REG_R) updates only GPR.UL[0]. Keep the
		// low-word pin in sync while preserving the existing high word.
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0 && static_cast<unsigned>(pin_host) != host_low &&
			!m_code.EmitMovRegShiftImm(static_cast<unsigned>(pin_host), host_low,
				VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		if (pin_host >= 0 && static_cast<unsigned>(pin_host) == host_low)
			g_qemuGprPinSelfStoresElided++;
#endif

		const bool deferred = TryDeferGprPinLowStore(guest_reg);
		if (!deferred &&
			!m_code.EmitStrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(GprOffset(guest_reg))))
		{
			return false;
		}

		InvalidateGprQCacheForGuest(guest_reg);
		return true;
	}

	bool BlockCompiler::TryEmitStoreGprLow64FromQCache(unsigned guest_reg, unsigned source_guest_reg, bool* emitted)
	{
		*emitted = false;
		if (guest_reg == 0 || source_guest_reg == 0)
			return true;

		const int cached_qreg = FindGprQCache(source_guest_reg);
		if (cached_qreg < 0)
			return true;

		const int pin_host = FindGprPinHost(guest_reg);
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		const bool needs_pin_sync = pin_host >= 0 || high_pin_host >= 0;
		const unsigned cached_qreg_u = static_cast<unsigned>(cached_qreg);

		if (pin_host >= 0 &&
			!EmitMoveQWordLaneToCore(static_cast<unsigned>(pin_host), cached_qreg_u, 0))
		{
			return false;
		}
		if (high_pin_host >= 0 &&
			!EmitMoveQWordLaneToCore(static_cast<unsigned>(high_pin_host), cached_qreg_u, 1))
		{
			return false;
		}

		if (!m_code.EmitVstrDImm(static_cast<unsigned>(cached_qreg) * 2,
				HOST_CPU_REGS, static_cast<u16>(GprOffset(guest_reg))))
		{
			return false;
		}

		*emitted = true;
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprQCacheDirectLow64CopyStores++;
		if (needs_pin_sync)
			g_qemuGprQCacheDirectLow64PinCopyStores++;
#endif
		InvalidateGprQCacheForGuest(guest_reg);
		return true;
	}

	bool BlockCompiler::TryEmitStoreGprLow64InvertFromQCache(unsigned guest_reg, unsigned source_guest_reg, bool* emitted)
	{
		*emitted = false;
		if (guest_reg == 0 || source_guest_reg == 0)
			return true;

		const int cached_qreg = FindGprQCache(source_guest_reg);
		if (cached_qreg < 0)
			return true;

		const unsigned cached_qreg_u = static_cast<unsigned>(cached_qreg);
		const unsigned temp_qreg = cached_qreg_u == 0 ? 1u : 0u;
		const int pin_host = FindGprPinHost(guest_reg);
		const int high_pin_host = FindGprPinHighHost(guest_reg);

		InvalidateGprQCacheForQreg(temp_qreg);
		if (!m_code.EmitVmvnQ(temp_qreg, cached_qreg_u))
			return false;

		if (pin_host >= 0 &&
			!EmitMoveQWordLaneToCore(static_cast<unsigned>(pin_host), temp_qreg, 0))
		{
			return false;
		}
		if (high_pin_host >= 0 &&
			!EmitMoveQWordLaneToCore(static_cast<unsigned>(high_pin_host), temp_qreg, 1))
		{
			return false;
		}

		if (!m_code.EmitVstrDImm(temp_qreg * 2,
				HOST_CPU_REGS, static_cast<u16>(GprOffset(guest_reg))))
		{
			return false;
		}

		*emitted = true;
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprQCacheDirectInvertStores++;
#endif
		InvalidateGprQCacheForGuest(guest_reg);
		return true;
	}

	bool BlockCompiler::EmitStoreGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high)
	{
		if (guest_reg == 0)
			return true;

		// Most blocks keep memory authoritative immediately. The vetted scalar
		// subset defers only pinned low/high words and flushes them at block exit.
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0 && static_cast<unsigned>(pin_host) != host_low &&
			!m_code.EmitMovRegShiftImm(static_cast<unsigned>(pin_host), host_low,
				VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		if (pin_host >= 0 && static_cast<unsigned>(pin_host) == host_low)
			g_qemuGprPinSelfStoresElided++;
#endif

		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0 && static_cast<unsigned>(high_pin_host) != host_high &&
			!m_code.EmitMovRegShiftImm(static_cast<unsigned>(high_pin_host), host_high,
				VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		if (high_pin_host >= 0 && static_cast<unsigned>(high_pin_host) == host_high)
			g_qemuGprPinSelfStoresElided++;
#endif

		const bool defer_low = TryDeferGprPinLowStore(guest_reg);
		const bool defer_high = TryDeferGprPinHighStore(guest_reg);
		const size_t offset = GprOffset(guest_reg);
		bool stored = false;
		[[maybe_unused]] u32 backing_store_instructions = 0;
		if (!defer_low && !defer_high && offset <= 0xff && CanUseA32DualTransferPair(host_low, host_high))
		{
			stored = m_code.EmitStrdImm8(host_low, host_high, HOST_CPU_REGS, static_cast<u8>(offset));
			backing_store_instructions = 1;
		}
		else
		{
			stored = true;
			if (!defer_low)
			{
				stored = m_code.EmitStrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(offset));
				backing_store_instructions++;
			}
			if (stored && !defer_high)
			{
				stored = m_code.EmitStrImm12(host_high, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
				backing_store_instructions++;
			}
		}

		if (!stored)
			return false;

#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGpr64BackingStoreInstructions += backing_store_instructions;
#endif

		InvalidateGprQCacheForGuest(guest_reg);
		return true;
	}
} // namespace VitaEE
