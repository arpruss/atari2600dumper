See wiring instructions in code.

The only bankswitching currently supported is F4, F6, F8, FA, FE and DPC.

I always use Roger's libmaple-based core rather than the official STM32 core, because USBComposite only works with libmaple, and libmaple is also less bloated.

Here are two different sets of build instructions (the second is mine) how to set up Arduino for this core:

 1. https://www.instructables.com/Programming-STM32-Blue-Pill-Via-USB/

 2. https://www.instructables.com/Gamecube-Controller-USB-Adapter-and-Getting-Starte/ (see steps 1 and 2)

I fear something may be out of date in these instructions, and a lot of developers have jumped ship from the libmaple core to the official one. People keep asking me to make USBComposite compatible with the official core, but I don't have the energy to figure out the details of how to do that.
