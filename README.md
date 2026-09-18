For developers only. This project serves little to no use for the majority of users.
# VitaSX2
VitaSX2 is a Vita-only redesign of PCSX2 tailor made for the Vita's limited hardware.
It keeps PCSX2's definition of PS2-visible behaviour, but replaces the machinery used to execute that model:
* Custom AArch32 recompilers for EE, IOP, VU and VIF work.
* Execution strategies designed around Cortex-A9’s limited registers, weak single-thread performance, small caches, and expensive synchronization—not mechanically translated desktop register allocation.
* Vita-specific memory arenas, executable-memory handling, cache maintenance, thread ownership, display queues, input, storage and lifecycle management.
* Experimental GPU-VU execution which attempts something more radical than a CPU dynarec: moving selected VU1/VIF/GS pipelines onto the SGX’s USSE execution resources.

## Expected FPS numbers:

VitaSX2 is a research emulator intended for developer use only. It does not play PS2 games at full speed and never will. Although, it can boot a surprising number of retail PS2 games.  

SCE SDK `gs`/ samples:
- sce__vu1__iga__sample: 2 FPS (8 FPS with VU1 running on GPU)
- sce__vu1__hako__sample: 60 FPS
- sce__vu1__blow__blow: 0 FPS
- sce__basic3d__core__core: 56 FPS
- sce__advanced__anti__aa1-feather__aa1: 21 FPS
- sce__graphics__anti__main: 14 FPS

PS2DEV SDK samples:
- cube: 60 FPS
- doublebuffer: 60 FPS
- draw: 60 FPS
- helloworld: 60 FPS
- teapot: 8 FPS
- vu1: 30 FPS

PlayStation 2 BIOS: 6 FPS (50 FPS+ ish in the Browser)  

Retail games:
* SLPM-66973 プリンセスナイトメア: 9 FPS
* SLES-55673 Pro Evolution Soccer 2014: 3 FPS
* SLPM-66302 CLANNAD: 7 FPS
* SLUS-20680 SpongeBob SquarePants - Battle for Bikini Bottom: 4 FPS
* SLUS-20591 Dragon Ball Z - Budokai: 2 FPS

## No more further optimization potential
There's no "just optimize it more" for VitaSX2. All optimizations have already been used. VitaSX2 is already an extremely well optimized emulator.   
VitaSX2 is already the fastest PS2 emulator that runs on AArch32 hardware, the hard bottleneck is the Vita's slow CPU.   
Optimizing further would yield a 1 or 2 FPS gain at best, not the 10x faster increase you'd want for a playable emulator. The SGX543MP4+ GPU is also the bottleneck in some workloads, notably the PS2 BIOS, with little to no optimization potential.  

This emulator already pushes the Vita to its theoretical limit.

## How to use

* Prepare a BIOS image in `ux0:data/vitasx2/bios/` as `bios.bin`.
* Set the boot path in `ux0:data/vitasx2/boot-path.txt` (file). To boot the BIOS, set the boot path to `bios`. To boot an .iso or .elf, edit `boot-path.txt` to the full path of the .iso or .elf image.

libshaccCg.suprx is required for GPU-VU compute shaders.  
kubridge.skprx is optionally required for slightly more EE executable hot regions (marginal performance gain).  
