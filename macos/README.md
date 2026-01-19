This is a guide to working with macOS for emulation.

IMPORTANT NOTES ABOUT WORKING WITH XENIA (from the author):
There seems to be a lot of misconceptions from some about how to handle building Xenia on the ARM64 Macs. These lead to less than ideal implementations of Xenia. Since not much is documented, I'd like to explain the old method implemented by Wunkolo. His backend relied upon not translating AVX and SSE instruction sets, but using oakwood, which handles a64 instructions. Ideally for a backend approach, this creates a giant overhead. Because a64 is a rather different instruction set that you must now make talk to the AVX instructions correctly, so you have a bunch more code overhead to deal with.

Given the good support already under Canary, there's a far better approach to avoiding this completely, and is what makes my backend code so tiny. What I do is work with x86 binaries, and make sure that AVX and other instructions are compliant for my environment using xsimd. 

LOC very much matter as we bring the emulator to full support, and this is referring to lines that add signicant complexity to compile time. Preprocessor definitions in which you comply to macOS data types is only preprocessor overhead that is worth the time, and should be weighed less significantly.

HOW TO CONTRIBUTE:
- When adding to this macOS foundation, any contribution you hope to make to my branch for merge requires you to check CI integration. Code that fails the CI, no matter how much it enables support for your Mac, is not acceptable for being integrated into this codebase

- Any contributions involving the creation of translation layers MUST be emailed to Ryan Wiseman: ryanwisemanmusic@gmail.com 
    Because of some less than ideal methods I've seen, we want to avoid architectural problems. This can include a variety of worrysome habits, for example, allowing warnings to build up and techincal debt to be accrued in the process of taking a shortcut. 

- You must enable support below the GPTK level. If you are unaware, Apple's Game Porting Toolkit allows for a layer of abstraction to happen that goes beyond just Rosetta 2 translations. And because of this, this means it is an assumed functionality on your system. Rosetta 2 is the only relevant compile overhead we need to deal with, as GPTK is another black box that can be a conflict with what is happening under Rosetta 2. Plus, it is better to deal with stuff like AVX support, and any x86 emulation, within Xenia itself, since these definitions will remain constant to a minimal library.

STATE OF NATIVE ARM64 SUPPORT;
ARM64 support for Xenia is something people want to deliver because it is a crucial portion in cross platform functinality. However, too many assume that slapping on oakwood is a good process in porting this code. This shortcut method is not appropriate, please do not insist that we "just add oakwood for the moment". This doesn't offer true native hardware support and will just require a bigger rewrite because it's offering emulation contained within emulation. A true ARM64 backend that appropriately handles things has not been developed. If you want to co-ordinate support around this topic for some day in the future, when the Mac port is in a good stable state, contact me at: ryanwisemanmusic@gmail.com

When you want to work with my build of Xenia Canary, here are a few commands that I find incredibly useful:
- To capture the build process and see what is going wrong:
        ./xb build 2>&1 | tee output.txt

- To do a full run on Canary:
        ( ./xb setup && ./xb premake && ./xb build ) 2>&1 | tee output.txt

- To run Xenia Canary:
        open build/bin/Mac/Checked/xenia_canary.app

- To debug Xenia Canary:
        lldb build/bin/Mac/Checked/xenia_canary.app/Contents/MacOS/xenia_canary
        build/bin/Mac/Checked/xenia_canary.app/Contents/MacOS/xenia_canary 2>&1 | tee xenia_canary.log     
