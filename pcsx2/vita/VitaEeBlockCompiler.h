// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <vector>

namespace VitaA32
{
	class CodeBuffer;
	enum class ShiftType : u8;
}

namespace VitaEE
{
	struct DirectLinkSlot
	{
		u32 target_pc = 0;
		size_t target_offset = 0;
		bool valid = false;
	};

	struct DirectLinkSlots
	{
		DirectLinkSlot slots[2]{};
	};

	void RefreshRawGpr0KnownZero();

	enum class SignedBranchCondition : u8
	{
		LessThanZero,
		GreaterEqualZero,
		LessEqualZero,
		GreaterThanZero,
	};

	class BlockCompiler
	{
		enum class MmiVectorOp : u8
		{
			AddWord,
			SubtractWord,
			CompareGreaterSignedWord,
			MaxSignedWord,
			AddHalfword,
			SubtractHalfword,
			CompareGreaterSignedHalfword,
			MaxSignedHalfword,
			AddByte,
			SubtractByte,
			CompareGreaterSignedByte,
			CompareEqualWord,
			MinSignedWord,
			CompareEqualHalfword,
			MinSignedHalfword,
			CompareEqualByte,
			SaturatingAddSignedWord,
			SaturatingSubtractSignedWord,
			SaturatingAddSignedHalfword,
			SaturatingSubtractSignedHalfword,
			SaturatingAddSignedByte,
			SaturatingSubtractSignedByte,
			SaturatingAddUnsignedWord,
			SaturatingSubtractUnsignedWord,
			SaturatingAddUnsignedHalfword,
			SaturatingSubtractUnsignedHalfword,
			SaturatingAddUnsignedByte,
			SaturatingSubtractUnsignedByte,
			BitwiseAnd,
			BitwiseXor,
			BitwiseOr,
			BitwiseNor,
		};

		enum class MmiUnaryVectorOp : u8
		{
			AbsoluteSignedWord,
			AbsoluteSignedHalfword,
		};

		enum class MmiImmediateShiftOp : u8
		{
			ShiftLeftHalfword,
			ShiftRightLogicalHalfword,
			ShiftRightArithmeticHalfword,
			ShiftLeftWord,
			ShiftRightLogicalWord,
			ShiftRightArithmeticWord,
		};

		enum class MmiVariableWordShiftOp : u8
		{
			ShiftLeftLogical,
			ShiftRightLogical,
			ShiftRightArithmetic,
		};

		enum class MmiHalfwordShuffleOp : u8
		{
			Pinth,
			Pinteh,
			Pexeh,
			Prevh,
			Pexch,
			Pcpyh,
		};

		enum class MmiWordShuffleOp : u8
		{
			Pcpyld,
			Pcpyud,
			Pexew,
			Prot3w,
			Pexcw,
		};

		enum class MmiFiveBitOp : u8
		{
			Expand,
			Pack,
		};

		enum class MmiUpperInterleaveOp : u8
		{
			Word,
			Halfword,
			Byte,
		};

		enum class ScalarLoadWidth : u8
		{
			Byte,
			Halfword,
			Word,
			Dword,
		};

		enum class ScalarStoreWidth : u8
		{
			Byte,
			Halfword,
			Word,
			Dword,
		};

	public:
		explicit BlockCompiler(VitaA32::CodeBuffer& code);

		static bool CanCompileOpcode(u32 op);
		static bool IsSupportedBranchOpcode(u32 op);
		static bool IsBranchLikely(u32 op);
		static bool CanCompileDelaySlotOpcode(u32 op);

		bool BeginBlock(bool use_vtlb_registers);
		bool CompileStraightLineBlock(u32 start_pc, u32 instruction_count, const void* direct_exit, const void* event_exit,
			u32* scaled_cycles = nullptr, DirectLinkSlots* direct_links = nullptr);
		bool EmitOpcode(u32 op, u32 pc = 0, u32 raw_cycles_through_instruction = 0,
			const void* event_exit = nullptr, bool branch_delay_slot = false);
		bool EndBlockReturn(u8 value);
		bool EndBlockWithCycleTest(u32 block_cycles, const void* direct_exit, const void* event_exit,
			size_t* direct_link_target_offset = nullptr, size_t* taken_link_target_offset = nullptr);
		bool EndBlockWithLikelyCycleTest(u32 taken_cycles, u32 not_taken_cycles, const void* direct_exit,
			const void* event_exit, size_t* not_taken_link_target_offset = nullptr,
			size_t* taken_link_target_offset = nullptr);
		static bool RequiresBlockEndAfterOpcode(u32 op);

	private:
		bool EmitAndImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags = false);
		bool EmitOrrImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags = false);
		bool EmitEorImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags = false);
		bool EmitBicImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags = false);
		bool EmitCmpImm32OrReg(unsigned rn, u32 value, unsigned scratch);
		bool EmitSPECIAL(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot);
		bool EmitCOP0(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitMFC0Fast(u32 op, u32 raw_cycles_through_instruction);
		bool EmitMFC0CountFast(u32 op, u32 scaled_cycles_through_instruction);
		bool EmitMFC0PerfCounterFast(u32 op, u32 scaled_cycles_through_instruction);
		bool EmitMTC0Fast(u32 op, u32 raw_cycles_through_instruction);
		bool EmitSetNextEventDelta4FromCurrentCycle();
		bool EmitEIEventExit(u32 op, u32 next_pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitERETEventExit(u32 op, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitTLBRInBlock();
		bool EmitTLBPInBlock();
		bool EmitDIDelayedStatusClear();
		bool EmitCOP1(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitCOP1MoveControlFast(u32 op);
		bool EmitCOP1ArithmeticFast(u32 op);
		bool EmitCOP1DivSqrtFast(u32 op);
		bool EmitCOP1AccumulatorFast(u32 op);
		bool EmitCOP1ScalarWordFast(u32 op);
		bool EmitCOP1CompareFast(u32 op);
		bool EmitCOP1ConvertWordFast(u32 op);
		bool EmitCOP1ConvertSingleFast(u32 op);
		bool EmitCOP2(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitCOP2VectorTransferEventExit(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit);
		bool EmitCOP2ControlReadEventExit(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit);
		bool EmitCOP2ControlWriteEventExit(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit);
		bool EmitCACHE(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitSpecialExceptionEventExit(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot, const void* helper);
		bool EmitSYSCALL(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit,
			bool branch_delay_slot);
		bool EmitBREAK(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit,
			bool branch_delay_slot);
		bool EmitTrapEventExit(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot);
		bool EmitADDIU(u32 op);
		bool EmitDADDIU(u32 op);
		bool EmitSLTI(u32 op);
		bool EmitSLTIU(u32 op);
		bool EmitANDI(u32 op);
		bool EmitORI(u32 op);
		bool EmitXORI(u32 op);
		bool EmitLUI(u32 op);
		bool EmitSLL(u32 op);
		bool EmitSRL(u32 op);
		bool EmitSRA(u32 op);
		bool EmitSLLV(u32 op);
		bool EmitSRLV(u32 op);
		bool EmitSRAV(u32 op);
		bool EmitMOVZ(u32 op);
		bool EmitMOVN(u32 op);
		bool EmitMULT(u32 op);
		bool EmitMULTU(u32 op);
		bool EmitMADD(u32 op);
		bool EmitMADDU(u32 op);
		bool EmitMADD1(u32 op);
		bool EmitMADDU1(u32 op);
		bool EmitMFHI1(u32 op);
		bool EmitMFLO1(u32 op);
		bool EmitMTHI1(u32 op);
		bool EmitMTLO1(u32 op);
		bool EmitMULT1(u32 op);
		bool EmitMULTU1(u32 op);
		bool EmitDIV(u32 op);
		bool EmitDIVU(u32 op);
		bool EmitDIV1(u32 op);
		bool EmitDIVU1(u32 op);
		bool EmitPLZCW(u32 op);
		bool EmitPMFHL(u32 op);
		bool EmitPMTHL(u32 op);
		bool EmitMFHI(u32 op);
		bool EmitMFLO(u32 op);
		bool EmitMTHI(u32 op);
		bool EmitMTLO(u32 op);
		bool EmitMFSA(u32 op);
		bool EmitMTSA(u32 op);
		bool EmitPSLLH(u32 op);
		bool EmitPSRLH(u32 op);
		bool EmitPSRAH(u32 op);
		bool EmitPSLLW(u32 op);
		bool EmitPSRLW(u32 op);
		bool EmitPSRAW(u32 op);
		bool EmitMMI(u32 op);
		bool EmitMMI0(u32 op);
		bool EmitMMI1(u32 op);
		bool EmitMMI2(u32 op);
		bool EmitMMI3(u32 op);
		bool EmitPMADDW(u32 op);
		bool EmitPMSUBW(u32 op);
		bool EmitPMADDH(u32 op);
		bool EmitPHMADH(u32 op);
		bool EmitPMSUBH(u32 op);
		bool EmitPHMSBH(u32 op);
		bool EmitPDIVW(u32 op);
		bool EmitPDIVUW(u32 op);
		bool EmitPDIVBW(u32 op);
		bool EmitPMADDUW(u32 op);
		bool EmitPMULTW(u32 op);
		bool EmitPMULTUW(u32 op);
		bool EmitPMULTH(u32 op);
		bool EmitPMFHI(u32 op);
		bool EmitPMFLO(u32 op);
		bool EmitPMTHI(u32 op);
		bool EmitPMTLO(u32 op);
		bool EmitMmiVectorOp(u32 op, MmiVectorOp operation);
		bool EmitMmiUnaryRtVectorOp(u32 op, MmiUnaryVectorOp operation);
		bool EmitMmiImmediateShiftOp(u32 op, MmiImmediateShiftOp operation);
		bool EmitMmiVariableWordShiftOp(u32 op, MmiVariableWordShiftOp operation);
		bool EmitMmiHalfwordShuffleOp(u32 op, MmiHalfwordShuffleOp operation);
		bool EmitMmiWordShuffleOp(u32 op, MmiWordShuffleOp operation);
		bool EmitMmiFiveBitOp(u32 op, MmiFiveBitOp operation);
		bool EmitMmiInterleaveOp(u32 op, MmiUpperInterleaveOp operation, bool upper_half);
		bool EmitMmiUpperInterleaveOp(u32 op, MmiUpperInterleaveOp operation);
		bool EmitMmiPackEvenOp(u32 op, MmiUpperInterleaveOp operation);
		bool EmitPADDW(u32 op);
		bool EmitPSUBW(u32 op);
		bool EmitPCGTW(u32 op);
		bool EmitPMAXW(u32 op);
		bool EmitPADDH(u32 op);
		bool EmitPSUBH(u32 op);
		bool EmitPCGTH(u32 op);
		bool EmitPMAXH(u32 op);
		bool EmitPADDB(u32 op);
		bool EmitPSUBB(u32 op);
		bool EmitPCGTB(u32 op);
		bool EmitPADDSW(u32 op);
		bool EmitPSUBSW(u32 op);
		bool EmitPEXTLW(u32 op);
		bool EmitPPACW(u32 op);
		bool EmitPADDSH(u32 op);
		bool EmitPSUBSH(u32 op);
		bool EmitPEXTLH(u32 op);
		bool EmitPPACH(u32 op);
		bool EmitPADDSB(u32 op);
		bool EmitPSUBSB(u32 op);
		bool EmitPEXTLB(u32 op);
		bool EmitPPACB(u32 op);
		bool EmitPEXT5(u32 op);
		bool EmitPPAC5(u32 op);
		bool EmitPABSW(u32 op);
		bool EmitPCEQW(u32 op);
		bool EmitPMINW(u32 op);
		bool EmitPADSBH(u32 op);
		bool EmitPABSH(u32 op);
		bool EmitPCEQH(u32 op);
		bool EmitPMINH(u32 op);
		bool EmitPCEQB(u32 op);
		bool EmitPADDUW(u32 op);
		bool EmitPSUBUW(u32 op);
		bool EmitPEXTUW(u32 op);
		bool EmitPADDUH(u32 op);
		bool EmitPSUBUH(u32 op);
		bool EmitPEXTUH(u32 op);
		bool EmitPADDUB(u32 op);
		bool EmitPSUBUB(u32 op);
		bool EmitPEXTUB(u32 op);
		bool EmitQFSRV(u32 op);
		bool EmitPSLLVW(u32 op);
		bool EmitPSRLVW(u32 op);
		bool EmitPSRAVW(u32 op);
		bool EmitPINTH(u32 op);
		bool EmitPCPYLD(u32 op);
		bool EmitPEXEH(u32 op);
		bool EmitPREVH(u32 op);
		bool EmitPEXEW(u32 op);
		bool EmitPROT3W(u32 op);
		bool EmitPINTEH(u32 op);
		bool EmitPCPYUD(u32 op);
		bool EmitPEXCH(u32 op);
		bool EmitPCPYH(u32 op);
		bool EmitPEXCW(u32 op);
		bool EmitPAND(u32 op);
		bool EmitPXOR(u32 op);
		bool EmitPOR(u32 op);
		bool EmitPNOR(u32 op);
		bool EmitREGIMM(u32 op, u32 pc);
		bool EmitMTSAB(u32 op);
		bool EmitMTSAH(u32 op);
		bool EmitJ(u32 op, u32 pc);
		bool EmitJAL(u32 op, u32 pc);
		bool EmitJR(u32 op, u32 pc);
		bool EmitJALR(u32 op, u32 pc);
		bool EmitBEQ(u32 op);
		bool EmitBNE(u32 op);
		bool EmitBLEZ(u32 op);
		bool EmitBGTZ(u32 op);
		bool EmitBEQL(u32 op);
		bool EmitBNEL(u32 op);
		bool EmitBLEZL(u32 op);
		bool EmitBGTZL(u32 op);
		bool EmitLB(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot);
		bool EmitLH(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot);
		bool EmitLW(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot);
		bool EmitLBU(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot);
		bool EmitLHU(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool branch_delay_slot);
		bool EmitLWU(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitLWL(u32 op);
		bool EmitLWR(u32 op);
		bool EmitLD(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitLDL(u32 op);
		bool EmitLDR(u32 op);
		bool EmitLQ(u32 op);
		bool EmitLWC1(u32 op);
		bool EmitLQC2(u32 op);
		bool EmitSB(u32 op);
		bool EmitSH(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitSW(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitSWL(u32 op);
		bool EmitSWR(u32 op);
		bool EmitSD(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitSDL(u32 op);
		bool EmitSDR(u32 op);
		bool EmitSQ(u32 op);
		bool EmitSWC1(u32 op);
		bool EmitSQC2(u32 op);
		bool EmitDSLLV(u32 op);
		bool EmitDSRLV(u32 op);
		bool EmitDSRAV(u32 op);
		bool EmitDSLL(u32 op);
		bool EmitDSRL(u32 op);
		bool EmitDSRA(u32 op);
		bool EmitDSLL32(u32 op);
		bool EmitDSRL32(u32 op);
		bool EmitDSRA32(u32 op);
		bool EmitADDU(u32 op);
		bool EmitSUBU(u32 op);
		bool EmitDADDU(u32 op);
		bool EmitDSUBU(u32 op);
		bool EmitAND(u32 op);
		bool EmitOR(u32 op);
		bool EmitXOR(u32 op);
		bool EmitNOR(u32 op);
		bool EmitSLT(u32 op);
		bool EmitSLTU(u32 op);
		bool EmitShift32Immediate(u32 op, VitaA32::ShiftType shift);
		bool EmitShift32Variable(u32 op, VitaA32::ShiftType shift);
		bool EmitShift64LeftImmediate(u32 op, unsigned amount);
		bool EmitShift64RightImmediate(u32 op, unsigned amount, bool arithmetic);
		bool EmitShift64LeftVariable(u32 op);
		bool EmitShift64RightVariable(u32 op, bool arithmetic);
		bool EmitConditionalMove(u32 op, bool move_on_zero);
		bool EmitMultiply(u32 op, bool signed_multiply, bool upper_pipeline = false);
		bool EmitMultiplyAdd(u32 op, bool signed_multiply, bool upper_pipeline);
		bool EmitPackedWordMultiply(u32 op, bool signed_multiply);
		bool EmitPackedSignedWordMultiplyAccumulate(u32 op, bool subtract);
		bool EmitPackedHalfwordMultiplyAccumulate(u32 op, bool subtract);
		bool EmitPackedHalfwordPairMultiply(u32 op, bool subtract);
		bool EmitPackedHalfwordMultiply(u32 op);
		bool EmitPackedUnsignedWordMultiplyAdd(u32 op);
		bool EmitScalarDivide(u32 op, bool signed_divide, bool upper_pipeline);
		bool EmitPackedWordDivide(u32 op, bool signed_divide);
		bool EmitPackedWordByHalfwordDivide(u32 op);
		bool EmitMoveFromHiLo(u32 op, size_t hilo_offset);
		bool EmitMoveToHiLo(u32 op, size_t hilo_offset);
		bool EmitMoveFullFromHiLo(u32 op, size_t hilo_offset);
		bool EmitMoveFullToHiLo(u32 op, size_t hilo_offset);
		bool EmitGoemonBlockStartHook(u32 start_pc);
		bool EmitLink(unsigned guest_reg, u32 pc);
		bool EmitJump(u32 pc, bool link);
		bool EmitRegisterJump(u32 op, u32 pc, bool link);
		bool EmitGoemonTranslateHostReg(unsigned host_reg);
		bool EmitCompareGpr64ForBranch(unsigned lhs_guest_reg, unsigned rhs_guest_reg);
		bool EmitBranchEqual(u32 op, bool branch_on_equal);
		bool EmitBranchSigned(u32 op, SignedBranchCondition condition);
		bool EmitCop0Branch(u32 op);
		bool EmitCop1Branch(u32 op);
		bool EmitCop2Branch(u32 op);
		bool EmitSetLessThan64(unsigned guest_reg, bool signed_compare);
		bool EmitSetLessThan64Imm(unsigned guest_reg, s32 imm, bool signed_compare);
		bool EmitLoadWithCounterReadEvent(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, const void* read_helper, bool sign_extend, unsigned sign_shift,
			bool branch_delay_slot, ScalarLoadWidth width, u8 alignment_mask, bool counter_read_event);
		bool EmitPartialWordLoad(u32 op, bool left);
		bool EmitPartialWordStore(u32 op, bool left);
		bool EmitPartialDwordLoad(u32 op, bool left);
		bool EmitPartialDwordStore(u32 op, bool left);
		bool EmitCounterReadFlagFromAddress(unsigned host_reg);
		bool EmitCounterReadEventExit(u32 next_pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitAddressErrorEventExit(u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool store);
		bool EmitSystemHelperEventExit(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* helper, const void* event_exit, bool request_cache_reset = false);
		bool FlushColdTails();
		bool EmitDeviceTracePreInstruction(u32 pc);
		bool EmitLoadCpuRegsU64(size_t offset, unsigned host_low, unsigned host_high, unsigned address_scratch);
		bool EmitStoreCpuRegsU64(size_t offset, unsigned host_low, unsigned host_high, unsigned address_scratch);
		bool EmitAddScaledCyclesToCpu(u32 cycles);
		bool EmitEffectiveAddress(u32 op, unsigned host_reg);
		bool EmitCpuRegsAddress(unsigned host_reg, size_t offset);
		bool EmitLoadRawGpr0KnownZeroFlag(unsigned host_reg);
		bool EmitRefreshRawGpr0KnownZeroFromLow64(unsigned low_reg, unsigned high_reg);
		bool EmitVu0SyncIfRunning();
		bool EmitVu0VfAddress(unsigned host_reg, unsigned vf_reg);
		bool EmitVu0ViAddress(unsigned host_reg, unsigned vi_reg);
		bool EmitAlignQwordAddress(unsigned host_reg, unsigned scratch_reg);
		bool EmitVtlbNonHandlerHostAddress(unsigned host_reg, unsigned vmap_reg, unsigned scratch_reg,
			size_t* handler_fallback_branch);
		bool EmitVtlbNonHandlerHostAddress128(unsigned host_reg, unsigned vmap_reg, unsigned scratch_reg,
			size_t* handler_fallback_branch);
		bool EmitLoadGprLow(unsigned guest_reg, unsigned host_reg);
		bool EmitLoadGprHigh(unsigned guest_reg, unsigned host_reg);
		bool EmitLoadGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high);
		bool EmitStorePcFromHostReg(unsigned host_reg);
		bool EmitStoreBranchPc(u32 target_pc, u32 fallthrough_pc);
		bool EmitStorePc(u32 pc);
		bool EmitStoreGprZero64(unsigned guest_reg);
		bool EmitStoreGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high);

		struct ScalarLoadColdTail
		{
			size_t unaligned_fallback = static_cast<size_t>(-1);
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			u32 pc = 0;
			u32 raw_cycles_through_instruction = 0;
			const void* event_exit = nullptr;
			const void* read_helper = nullptr;
			ScalarLoadWidth width = ScalarLoadWidth::Byte;
			unsigned rt = 0;
			unsigned sign_shift = 0;
			bool sign_extend = false;
			bool branch_delay_slot = false;
			bool counter_read_event = false;
			unsigned address_reg = 0;
		};

		bool EmitScalarLoadColdTail(const ScalarLoadColdTail& tail);

		struct ScalarStoreColdTail
		{
			size_t unaligned_fallback = static_cast<size_t>(-1);
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			u32 pc = 0;
			u32 raw_cycles_through_instruction = 0;
			const void* event_exit = nullptr;
			const void* write_helper = nullptr;
			unsigned rt = 0;
			ScalarStoreWidth width = ScalarStoreWidth::Byte;
		};
		bool EmitScalarStoreColdTail(const ScalarStoreColdTail& tail);

		struct QwordLoadColdTail
		{
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			const void* read_helper = nullptr;
			unsigned rt = 0;
		};
		bool EmitQwordLoadColdTail(const QwordLoadColdTail& tail);

		struct QwordStoreColdTail
		{
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			const void* write_helper = nullptr;
			unsigned rt = 0;
		};
		bool EmitQwordStoreColdTail(const QwordStoreColdTail& tail);

		struct Cop1WordMemoryColdTail
		{
			size_t unaligned_fallback = static_cast<size_t>(-1);
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			const void* helper = nullptr;
			unsigned rt = 0;
		};
		bool EmitCop1WordMemoryColdTail(const Cop1WordMemoryColdTail& tail);

		struct Cop2QwordMemoryColdTail
		{
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			const void* helper = nullptr;
			unsigned rt = 0;
		};
		bool EmitCop2QwordMemoryColdTail(const Cop2QwordMemoryColdTail& tail);

		struct PartialMemoryColdTail
		{
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			const void* helper = nullptr;
			unsigned rt = 0;
		};
		bool EmitPartialMemoryColdTail(const PartialMemoryColdTail& tail);

		VitaA32::CodeBuffer& m_code;
		std::vector<ScalarLoadColdTail> m_scalar_load_cold_tails;
		std::vector<ScalarStoreColdTail> m_scalar_store_cold_tails;
		std::vector<QwordLoadColdTail> m_qword_load_cold_tails;
		std::vector<QwordStoreColdTail> m_qword_store_cold_tails;
		std::vector<Cop1WordMemoryColdTail> m_cop1_word_memory_cold_tails;
		std::vector<Cop2QwordMemoryColdTail> m_cop2_qword_memory_cold_tails;
		std::vector<PartialMemoryColdTail> m_partial_memory_cold_tails;
		u16 m_saved_registers = 0;
		bool m_vtlb_registers_available = false;
		};
	} // namespace VitaEE
