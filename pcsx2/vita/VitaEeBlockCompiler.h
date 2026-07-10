// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstddef>
#include <vector>

namespace VitaA32
{
	class CodeBuffer;
	enum class Condition : u8;
	enum class ShiftType : u8;
}

namespace VitaEE
{
	struct DirectLinkSlot
	{
		u32 target_pc = 0;
		size_t target_offset = static_cast<size_t>(-1);
		size_t fallback_offset = static_cast<size_t>(-1);
		bool branch_on_taken = false;
		bool patched_to_resident_entry = false;
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
		// Private EE chain ABI: r3 is alignment padding; r4-r11 and LR/PC are
		// saved by either the callable block prologue or the persistent dispatcher.
		static constexpr u16 LINK_FRAME_REGISTER_MASK = 0x0ff8u;

		explicit BlockCompiler(VitaA32::CodeBuffer& code);

		static bool CanCompileOpcode(u32 op);
		static bool IsSupportedBranchOpcode(u32 op);
		static bool IsBranchLikely(u32 op);
		static bool CanCompileDelaySlotOpcode(u32 op);

		bool BeginBlock(bool use_vtlb_registers = false, bool use_cop1_exponent_mask_register = false,
			bool use_vu0_base_register = false, size_t* linked_entry_offset = nullptr,
			u32 linked_entry_pc = 0, bool linked_entry_needs_pc_sync = false);
		bool CompileStraightLineBlock(u32 start_pc, u32 instruction_count, const void* direct_exit, const void* event_exit,
			u32* scaled_cycles = nullptr, DirectLinkSlots* direct_links = nullptr,
			const void* indirect_lookup_pages_slot = nullptr, const void* direct_linking_enabled_flag = nullptr,
			size_t* linked_entry_offset = nullptr, bool persistent_dispatch_exits = false,
			size_t* resident_self_link_entry_offset = nullptr,
			u8* resident_self_link_entry_loads = nullptr);
		bool EmitOpcode(u32 op, u32 pc = 0, u32 raw_cycles_through_instruction = 0,
			const void* event_exit = nullptr, bool branch_delay_slot = false);
		bool EndBlockReturn(u8 value);
		bool EndBlockWithCycleTest(u32 block_cycles, const void* direct_exit, const void* event_exit,
			DirectLinkSlot* direct_link = nullptr, DirectLinkSlot* taken_link = nullptr,
			const void* indirect_lookup_pages_slot = nullptr, const void* direct_linking_enabled_flag = nullptr,
			bool wait_loop_taken = false, bool defer_pc_writeback = false,
			u32 direct_pc = 0, u32 taken_pc = 0, bool conditional_pc = false,
			bool indirect_pc_writeback = false, bool preserve_dirty_taken_self_link = false);
		bool EndBlockWithLikelyCycleTest(u32 taken_cycles, u32 not_taken_cycles, const void* direct_exit,
			const void* event_exit, DirectLinkSlot* not_taken_link = nullptr,
			DirectLinkSlot* taken_link = nullptr, bool wait_loop_taken = false,
			bool defer_pc_writeback = false, u32 not_taken_pc = 0, u32 taken_pc = 0);
		static bool RequiresBlockEndAfterOpcode(u32 op);

	private:
		bool EmitLinkFrameReturn();
		bool EmitExitToTarget(const void* target, u8 callable_token);
		bool EmitAndImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags = false);
		bool EmitOrrImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags = false);
		bool EmitEorImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags = false);
		bool EmitBicImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags = false);
		bool EmitCmpImm32OrReg(unsigned rn, u32 value, unsigned scratch);
		bool EmitCmpImm32OrReg(unsigned rn, u32 value, unsigned scratch, VitaA32::Condition condition);
		bool EmitDirectLinkTail(const void* direct_exit, DirectLinkSlot* direct_link,
			bool defer_pc_writeback = false, u32 pc = 0);
		bool EmitTakenDirectLinkTail(const void* direct_exit, size_t target_branch,
			DirectLinkSlot* direct_link, bool defer_pc_writeback = false, u32 pc = 0,
			bool sync_dirty_fallback = false);
		bool EmitIndirectDispatchTail(const void* lookup_pages_slot, const void* direct_linking_enabled_flag,
			const void* direct_exit, bool defer_pc_writeback = false);
		bool EmitEventExitReturn(const void* event_exit);
		bool EmitDeferredPcWriteback(bool defer_pc_writeback, u32 direct_pc, u32 taken_pc,
			bool conditional_pc, bool indirect_pc_writeback = false);
		static bool IsWaitLoopBody(u32 loop_start_pc, u32 loop_end_pc, u32 branch_pc);
		bool EmitWaitLoopFastForwardTail(const void* event_exit,
			bool defer_pc_writeback = false, u32 pc = 0);
		bool EndBlockWithWaitLoopFastForward(u32 block_cycles, const void* event_exit);
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
		bool EmitCOP2IdleBranch(size_t* vu0_idle);
		bool EmitCOP2VectorTransferBody(u32 op);
		bool EmitCOP2ControlReadBody(u32 op);
		bool EmitCOP2ControlWriteBody(u32 op);
		bool EmitCOP2MacroCodeWrite(u32 op);
		bool EmitCOP2MacroCopySelectedLanes(unsigned mask, unsigned source_address_reg,
			unsigned dest_address_reg, unsigned temp_qreg);
		bool EmitCOP2MacroStoreSelectedLanes(unsigned mask, unsigned value_qreg, unsigned address_reg);
		bool EmitCOP2MacroStoreVfSelectedLanes(unsigned vf_reg, unsigned mask, unsigned value_qreg,
			unsigned address_reg);
		bool EmitCOP2MacroBody(u32 op);
		bool EmitCOP2MacroArithmeticBody(u32 op);
		bool EmitVu0ViBackup(unsigned vi_reg);
		bool EmitCOP2MacroViBody(u32 op);
		bool EmitCOP2MacroViTransferBody(u32 op);
		bool EmitVu0RandomAdvance();
		bool EmitCOP2MacroRandomBody(u32 op);
		bool EmitVu0IndexedMemoryAddress(unsigned vi_reg);
		bool EmitCOP2MacroIndexedViMemoryBody(u32 op);
		bool EmitVu0ViLowHalfwordAdjust(unsigned vi_reg, bool decrement);
		bool EmitCOP2MacroIndexedVectorMemoryBody(u32 op);
		bool EmitCOP2MacroClipBody(u32 op);
		bool EmitCOP2MacroItofBody(u32 op);
		bool EmitCOP2MacroFtoiBody(u32 op);
		bool EmitCOP2MacroFdivBody(u32 op);
		bool EmitCOP2MacroMoveBody(u32 op);
		bool EmitCOP2MacroMinMaxBody(u32 op);
		bool EmitCOP2MacroFast(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit);
		bool EmitCOP2InterlockCall(u32 op, bool wait_for_mbit);
		bool EmitCOP2VectorTransferFast(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit);
		bool EmitCOP2ControlReadFast(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit);
		bool EmitCOP2ControlWriteFast(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
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
		bool EmitShift32Immediate(u32 op, VitaA32::ShiftType shift, unsigned amount);
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
		bool EmitCompareGpr64WithKnownForBranch(unsigned guest_reg, u32 low, u32 high);
		bool EmitCompareGpr64ForBranch(unsigned lhs_guest_reg, unsigned rhs_guest_reg);
		bool TryEvaluateConstantBranch(u32 op, bool* taken) const;
		bool TryEvaluateConstantRegimmLinkBranch(u32 op, bool* taken) const;
		bool EmitBranchEqual(u32 op, bool branch_on_equal);
		bool EmitBranchSigned(u32 op, SignedBranchCondition condition);
		bool EmitCop0Branch(u32 op);
		bool EmitCop1Branch(u32 op);
		bool EmitCop2Branch(u32 op);
		bool EmitSetLessThan64(unsigned guest_reg, bool signed_compare, unsigned lhs_low,
			unsigned lhs_high, unsigned rhs_low, unsigned rhs_high);
		bool EmitSetLessThan64Known(unsigned guest_reg, bool signed_compare,
			unsigned runtime_guest_reg, u32 known_low, u32 known_high, bool known_is_lhs);
		bool EmitSetLessThan64Imm(unsigned guest_reg, s32 imm, bool signed_compare,
			unsigned lhs_low, unsigned lhs_high);
		bool EmitLoadWithCounterReadEvent(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, const void* read_helper, bool sign_extend,
			bool branch_delay_slot, ScalarLoadWidth width, u8 alignment_mask, bool counter_read_event);
		bool EmitPartialWordLoad(u32 op, bool left);
		bool EmitPartialWordStore(u32 op, bool left);
		bool EmitPartialDwordLoad(u32 op, bool left);
		bool EmitPartialDwordStore(u32 op, bool left);
		bool EmitCounterReadFlagFromAddress(unsigned host_reg);
		bool EmitCounterReadEventExit(u32 next_pc, u32 raw_cycles_through_instruction, const void* event_exit);
		struct GprPinDirtyMasks
		{
			u8 low = 0;
			u8 high = 0;
		};
		bool EmitAddressErrorEventExit(u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit, bool store, const GprPinDirtyMasks& dirty_pins);
		bool EmitSystemHelperEventExit(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* helper, const void* event_exit, bool request_cache_reset = false);
		bool FlushColdTails();
		void ClearGprConstState();
		void ClearSaConstState();
		void ClearCop1NormalizedState();
		bool IsCop1FprNormalized(unsigned fpr) const;
		bool IsCop1AccNormalized() const;
		bool TryGetKnownGprLow(unsigned guest_reg, u32* value) const;
		bool TryGetKnownGpr64(unsigned guest_reg, u32* low, u32* high) const;
		bool EmitStoreKnownSignExtended32(unsigned guest_reg, u32 value);
		bool EmitStoreKnownZeroExtended32(unsigned guest_reg, u32 value);
		bool EmitStoreKnown64(unsigned guest_reg, u32 low, u32 high);
		bool EmitStoreGprSignExtended32FromLow(unsigned guest_reg, unsigned host_low);
		bool EmitStoreGprZeroExtended32FromLow(unsigned guest_reg, unsigned host_low);
		unsigned SelectGprLowResultHost(unsigned guest_reg, unsigned fallback_host);
		unsigned SelectGprHighResultHost(unsigned guest_reg, unsigned fallback_host);
		void UpdateCop1NormalizedStateAfterOpcode(u32 op);
		bool TryGetKnownEffectiveAddress(u32 op, u32* address) const;
		bool BlockNeedsResidentVtlbRegisters(u32 start_pc, u32 instruction_count);
		enum class KnownVtlbFastPathKind : u8
		{
			Scalar,
			Qword,
			Cop1,
			Cop2,
			Partial,
		};
		bool TryEmitKnownVtlbNonHandlerHostAddress(u32 guest_addr, unsigned host_reg,
			KnownVtlbFastPathKind kind = KnownVtlbFastPathKind::Scalar);
		void UpdateGprConstStateAfterOpcode(u32 op, u32 pc);
		void StageGprPinsForBlock(u32 start_pc, u32 instruction_count, bool allow_r5,
			bool allow_r7, bool allow_r8,
			bool allow_r10, bool allow_r11, bool prefer_dirty_writes,
			bool preserve_self_link_state);
		void StageGprQCacheForBlock(u32 start_pc, u32 instruction_count);
		bool BlockWritesPinnedGpr(u32 start_pc, u32 instruction_count) const;
		void MarkGprPinsDirtyAtResidentSelfLinkEntry(u32 start_pc, u32 instruction_count);
		u8 GprPinEntryLoadInstructionCount() const;
		bool EmitGprPinLoads();
		bool EmitGprQCacheEntryLoads();
		bool EmitFlushDirtyGprPins();
		bool EmitFlushDirtyGprPinsForGuest(unsigned guest_reg);
		GprPinDirtyMasks CurrentGprPinDirtyMasks() const;
		bool EmitSyncGprPinsToBacking(const GprPinDirtyMasks* dirty_pins = nullptr);
		bool EmitSyncForwardedBooleanBranchToBacking();
		bool EmitStageResidentCycleLow();
		bool EmitSyncResidentCycleLowToBacking();
		bool IsForwardedBooleanBranchResult(unsigned guest_reg) const;
		bool EmitStageResidentRawGpr0Qword();
		bool EmitStageResidentVtlbQwordPointer();
		int FindGprPinIndex(unsigned guest_reg) const;
		int FindGprPinHost(unsigned guest_reg) const;
		int FindGprPinHighHost(unsigned guest_reg) const;
		bool TryDeferGprPinLowStore(unsigned guest_reg);
		bool TryDeferGprPinHighStore(unsigned guest_reg);
		void ClearGprQCache();
		void InvalidateGprQCacheForGuest(unsigned guest_reg);
		void InvalidateGprQCacheForQreg(unsigned qreg);
		void MarkGprQCache(unsigned guest_reg, unsigned qreg);
		int FindGprQCache(unsigned guest_reg) const;
		bool IsGprQCacheQregResident(unsigned qreg) const;
		bool GprQCacheGuestHasFutureQwordReadBeforeWrite(unsigned guest_reg) const;
		bool GprQCacheGuestHasFutureQfsrvSourceReadBeforeWrite(unsigned guest_reg) const;
		bool GprQCacheQregHasFutureQwordReadBeforeWrite(unsigned qreg) const;
		u32 GprQCacheGuestNextQwordReadDistanceBeforeWrite(unsigned guest_reg) const;
		u32 GprQCacheQregNextQwordReadDistanceBeforeWrite(unsigned qreg) const;
		unsigned SelectGprQCacheScratchQreg(u32 avoid_qreg_mask = 0) const;
		bool GprQCacheGuestDefinedBeforeCurrentInstruction(unsigned guest_reg) const;
		u16 GprQCacheGuestEntryQwordReadCount(unsigned guest_reg) const;
		bool PreserveGprQCacheGuestForFutureRead(unsigned guest_reg, unsigned cached_qreg,
			unsigned avoid_qreg0, unsigned avoid_qreg1, bool* preserved);
		bool EmitStoreGprQ128PreservingCachedSourceIfFutureRead(unsigned dest_guest_reg,
			unsigned source_guest_reg, unsigned cached_qreg, unsigned address_scratch, bool* preserved);
		bool EmitDeviceTracePreInstruction(u32 pc);
		bool EmitLoadCpuRegsU64(size_t offset, unsigned host_low, unsigned host_high, unsigned address_scratch);
		bool EmitStoreCpuRegsU64(size_t offset, unsigned host_low, unsigned host_high, unsigned address_scratch);
		bool EmitAddScaledCyclesToCpuLowWord(u32 cycles, unsigned host_low, unsigned scratch,
			size_t* carry_branch);
		bool EmitCycleCarryFixup(const size_t* carry_branches, size_t carry_branch_count,
			size_t resume_offset, unsigned scratch);
		bool EmitLoadCpuRegsQ128(size_t offset, unsigned qreg, unsigned address_scratch);
		bool EmitStoreCpuRegsQ128(size_t offset, unsigned qreg, unsigned address_scratch);
		bool EmitMoveQWordLaneToCore(unsigned host_reg, unsigned qreg, unsigned word);
		bool EmitMoveQWordLaneToCore(unsigned host_reg, unsigned qreg, unsigned word,
			VitaA32::Condition condition);
		bool EmitMoveCoreToQWordLane(unsigned qreg, unsigned word, unsigned host_reg);
		bool EmitMoveCoreToQWordLane(unsigned qreg, unsigned word, unsigned host_reg,
			VitaA32::Condition condition);
		bool EmitMoveQWordLane(unsigned dest_qreg, unsigned dest_word, unsigned source_qreg,
			unsigned source_word, unsigned host_scratch);
		bool EmitLoadQWordLane(unsigned qreg, unsigned word, unsigned address_reg, u16 offset,
			unsigned host_scratch);
		bool EmitStoreQWordLane(unsigned qreg, unsigned word, unsigned address_reg, u16 offset,
			unsigned host_scratch);
		bool EmitCop1ExponentMask(unsigned host_reg);
		bool EmitAndCop1ExponentMask(unsigned rd, unsigned rn, unsigned scratch);
		bool EmitAndCop1FractionMask(unsigned rd, unsigned rn);
		bool EmitAddScaledCyclesToCpu(u32 cycles);
		bool EmitEffectiveAddress(u32 op, unsigned host_reg);
		bool EmitCpuRegsAddress(unsigned host_reg, size_t offset);
		bool EmitLoadRawGpr0KnownZeroFlag(unsigned host_reg);
		bool EmitRefreshRawGpr0KnownZeroFromLow64(unsigned low_reg, unsigned high_reg);
		bool EmitVu0SyncIfRunning(unsigned preserve_reg = 16, unsigned save_reg = 16);
		bool EmitVu0RegisterAddress(unsigned host_reg, size_t offset);
		bool EmitVu0ClipflagAddress(unsigned host_reg);
		bool EmitVu0Vf0ConstantQ(unsigned qreg, unsigned host_scratch);
		bool EmitVu0VfAddress(unsigned host_reg, unsigned vf_reg);
		bool EmitVu0ViAddress(unsigned host_reg, unsigned vi_reg);
		bool EmitAlignQwordAddress(unsigned host_reg, unsigned scratch_reg);
		bool EmitVtlbNonHandlerHostAddress(unsigned host_reg, unsigned vmap_reg, unsigned scratch_reg,
			size_t* handler_fallback_branch, GprPinDirtyMasks* dirty_pins = nullptr);
		bool EmitVtlbNonHandlerHostAddress128(unsigned host_reg, unsigned vmap_reg, unsigned scratch_reg,
			size_t* handler_fallback_branch, GprPinDirtyMasks* dirty_pins = nullptr);
		bool EmitLoadGprLow(unsigned guest_reg, unsigned host_reg);
		bool EmitLoadGprLowRawZero(unsigned guest_reg, unsigned host_reg);
		bool EmitGprLowOperand(unsigned guest_reg, unsigned fallback_host, unsigned* operand_host);
		bool EmitLoadGprLowKnownValue(unsigned guest_reg, unsigned host_reg, bool value_known, u32 value);
		bool EmitLoadGprLowValue(unsigned guest_reg, unsigned host_reg);
		bool EmitGprLowValueOperand(unsigned guest_reg, unsigned fallback_host, unsigned* operand_host);
		bool EmitLoadPartialStoreLowValue(unsigned guest_reg, unsigned host_reg);
		bool EmitLoadPartialStoreLowKnownValue(unsigned guest_reg, unsigned host_reg,
			bool value_known, u32 value);
		bool EmitLoadPartialDwordStoreByteValue(unsigned guest_reg, unsigned source_byte,
			unsigned host_reg, bool value_known, u32 low, u32 high);
		bool EmitRefreshGprPinFromBacking(unsigned guest_reg);
		bool TryEmitLoadGprWordFromQCache(unsigned guest_reg, unsigned word, unsigned host_reg,
			bool* emitted);
		bool EmitLoadGprWord(unsigned guest_reg, unsigned word, unsigned host_reg);
		bool EmitGprWordOperand(unsigned guest_reg, unsigned word, unsigned fallback_host,
			unsigned* operand_host);
		bool EmitGpr64ReadOperands(unsigned guest_reg, unsigned fallback_low, unsigned fallback_high,
			unsigned* low_operand_host, unsigned* high_operand_host);
		bool EmitLoadGpr64KnownValue(unsigned guest_reg, unsigned host_low, unsigned host_high,
			bool value_known, u32 low, u32 high);
		bool EmitLoadGpr64Value(unsigned guest_reg, unsigned host_low, unsigned host_high);
		bool EmitGpr64ValueReadOperands(unsigned guest_reg, unsigned fallback_low, unsigned fallback_high,
			unsigned* low_operand_host, unsigned* high_operand_host);
		bool EmitLoadGprHigh(unsigned guest_reg, unsigned host_reg);
		bool EmitLoadGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high);
		bool EmitLoadGprQ128(unsigned guest_reg, unsigned qreg, unsigned address_scratch);
		bool ShouldLoadGprQ128SingleUseEntry(unsigned guest_reg) const;
		bool EmitLoadGprQ128SingleUseEntry(unsigned guest_reg, unsigned qreg,
			unsigned address_scratch);
		bool EmitStorePcFromHostReg(unsigned host_reg);
		bool EmitStoreBranchPc(u32 target_pc, u32 fallthrough_pc);
		bool EmitStorePc(u32 pc);
		bool EmitStoreGprQ128(unsigned guest_reg, unsigned qreg, unsigned address_scratch);
		bool EmitStoreGprQ128ToAddress(unsigned guest_reg, unsigned qreg, unsigned address_reg);
		bool EmitStoreGprDwordPair(unsigned guest_reg, unsigned low_d, unsigned high_d);
		bool EmitStoreGprWord(unsigned guest_reg, unsigned word, unsigned host_reg);
		bool EmitStoreGprLowPreserveHigh(unsigned guest_reg, unsigned host_low);
		bool TryEmitStoreGprLow64FromQCache(unsigned guest_reg, unsigned source_guest_reg, bool* emitted);
		bool TryEmitStoreGprLow64InvertFromQCache(unsigned guest_reg, unsigned source_guest_reg, bool* emitted);
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
			bool sign_extend = false;
			bool branch_delay_slot = false;
			bool counter_read_event = false;
			unsigned address_reg = 0;
			GprPinDirtyMasks dirty_pins{};
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
			bool rt_low_known = false;
			bool rt_high_known = false;
			u32 rt_low = 0;
			u32 rt_high = 0;
			GprPinDirtyMasks dirty_pins{};
		};
		void CaptureScalarStoreValue(ScalarStoreColdTail* tail);
		bool EmitScalarStoreColdTail(const ScalarStoreColdTail& tail);

		struct QwordLoadColdTail
		{
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			unsigned rt = 0;
			GprPinDirtyMasks dirty_pins{};
		};
		bool EmitQwordLoadColdTail(const QwordLoadColdTail& tail);

		struct QwordStoreColdTail
		{
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			unsigned rt = 0;
			GprPinDirtyMasks dirty_pins{};
		};
		bool EmitQwordStoreColdTail(const QwordStoreColdTail& tail);

		struct Cop1WordMemoryColdTail
		{
			size_t unaligned_fallback = static_cast<size_t>(-1);
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			unsigned rt = 0;
			bool store = false;
			GprPinDirtyMasks dirty_pins{};
		};
		bool EmitCop1WordMemoryColdTail(const Cop1WordMemoryColdTail& tail);

		struct Cop2QwordMemoryColdTail
		{
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			unsigned rt = 0;
			bool store = false;
		};
		bool EmitCop2QwordMemoryColdTail(const Cop2QwordMemoryColdTail& tail);

		struct Vu0SyncColdTail
		{
			size_t running_branch = static_cast<size_t>(-1);
			size_t join_offset = 0;
			unsigned preserve_reg = 16;
			unsigned save_reg = 16;
		};
		bool EmitVu0SyncColdTail(const Vu0SyncColdTail& tail);

		enum class PartialMemoryOp : u8
		{
			WordLoadLeft,
			WordLoadRight,
			WordStoreLeft,
			WordStoreRight,
			DwordLoadLeft,
			DwordLoadRight,
			DwordStoreLeft,
			DwordStoreRight,
		};

		struct PartialMemoryColdTail
		{
			size_t handler_fallback = static_cast<size_t>(-1);
			size_t join_offset = 0;
			PartialMemoryOp op = PartialMemoryOp::WordLoadLeft;
			unsigned rt = 0;
			bool rt_low_known = false;
			u32 rt_low = 0;
			bool rt_high_known = false;
			u32 rt_high = 0;
			GprPinDirtyMasks dirty_pins{};
		};
		void CapturePartialStoreValue(PartialMemoryColdTail* tail);
		bool EmitPartialMemoryColdTail(const PartialMemoryColdTail& tail);

		VitaA32::CodeBuffer& m_code;
		std::vector<ScalarLoadColdTail> m_scalar_load_cold_tails;
		std::vector<ScalarStoreColdTail> m_scalar_store_cold_tails;
		std::vector<QwordLoadColdTail> m_qword_load_cold_tails;
		std::vector<QwordStoreColdTail> m_qword_store_cold_tails;
		std::vector<Cop1WordMemoryColdTail> m_cop1_word_memory_cold_tails;
		std::vector<Cop2QwordMemoryColdTail> m_cop2_qword_memory_cold_tails;
		std::vector<Vu0SyncColdTail> m_vu0_sync_cold_tails;
		std::vector<PartialMemoryColdTail> m_partial_memory_cold_tails;
		u16 m_saved_registers = 0;
		bool m_vtlb_registers_available = false;
		bool m_cop1_exponent_mask_available = false;
		bool m_vu0_base_available = false;
			static constexpr unsigned MAX_GPR_PINS = 5;
			static_assert(MAX_GPR_PINS <= 8);
			// Per-block read pins: guest GPR low words held in callee-saved host
			// registers for the whole block, optionally with a companion high word
			// for hot low64 scalar state. Most blocks remain write-through; a narrow
			// scalar/control plus scalar-memory subset defers pinned stores and flushes
			// at block exits or before helper/event cold seams. Persistent self-links
			// may re-enter after these loads because the exact same mapping remains
			// live; all other incoming edges rebuild it from backing state.
		u8 m_staged_pin_guest[MAX_GPR_PINS]{};
		u8 m_staged_pin_host[MAX_GPR_PINS]{};
		u8 m_staged_pin_high_host[MAX_GPR_PINS]{};
		bool m_staged_pin_needs_entry_load[MAX_GPR_PINS]{};
		u8 m_staged_pin_count = 0;
		u8 m_pin_guest[MAX_GPR_PINS]{};
		u8 m_pin_host[MAX_GPR_PINS]{};
		u8 m_pin_high_host[MAX_GPR_PINS]{};
		bool m_pin_needs_entry_load[MAX_GPR_PINS]{};
		u8 m_pin_count = 0;
		bool m_dirty_pins_enabled = false;
		bool m_pin_dirty_low[MAX_GPR_PINS]{};
		bool m_pin_dirty_high[MAX_GPR_PINS]{};
		bool m_gpr_const_known[32]{};
		u32 m_gpr_const_low[32]{};
		bool m_gpr_const_high_known[32]{};
		u32 m_gpr_const_high[32]{};
		bool m_sa_const_known = false;
		u8 m_sa_const_byte_offset = 0;
		bool m_cop1_fpr_normalized[32]{};
		bool m_cop1_acc_normalized = false;
		// True once the vuDouble() bit-select constant quads (physical Q8-Q11)
		// have been materialized in this block. Physical Q12-Q15 are ephemeral
		// normalize scratch in COP2 macro blocks and back the private ABI's logical
		// Q4-Q7 bank in qcache blocks; the block classifier makes those roles
		// mutually exclusive. Reset per block in BeginBlock().
		bool m_cop2_norm_consts_ready = false;
		bool m_gpr_q_cache_enabled = false;
		bool m_persistent_dispatch_exits = false;
		u8 m_branch_flag_host = 0;
		bool m_forwarded_boolean_branch = false;
		u8 m_forwarded_boolean_guest = 0;
		u32 m_forwarded_boolean_producer_index = 0;
		bool m_resident_raw_gpr0_qword = false;
		u8 m_resident_raw_gpr0_entry_instructions = 0;
		bool m_resident_vtlb_qword_pointer = false;
		bool m_resident_cycle_low = false;
		u32 m_resident_vtlb_qword_store_op = 0;
		size_t m_resident_vtlb_qword_guard_offset = static_cast<size_t>(-1);
		size_t m_resident_vtlb_qword_handler_fallback = static_cast<size_t>(-1);
		GprPinDirtyMasks m_resident_vtlb_qword_dirty_pins{};
		u8 m_resident_vtlb_qword_guard_instructions = 0;
		u8 m_resident_vtlb_qword_translation_instructions = 0;
		u32 m_current_opcode = 0;
		u32 m_current_block_start_pc = 0;
		u32 m_current_block_instruction_count = 0;
		u32 m_current_instruction_index = 0;
		static constexpr unsigned MAX_GPR_QCACHE = 8;
		u8 m_gpr_q_cache_guest[MAX_GPR_QCACHE]{};
		u8 m_gpr_q_cache_qreg[MAX_GPR_QCACHE]{};
		u8 m_gpr_q_cache_count = 0;
		u8 m_staged_gpr_q_cache_guest[MAX_GPR_QCACHE]{};
		u8 m_staged_gpr_q_cache_count = 0;
		std::vector<u16> m_gpr_q_cache_next_use_distances;
		};
	} // namespace VitaEE
