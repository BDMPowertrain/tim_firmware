/* Extra linker script, given after each image's CubeMX-generated one.

   ADR 0014: the image is confined to bank 1 and limited to 224 KB. Bank 2
   is left for stored data, and the other 32 KB of bank 1 is the future
   bootloader's. Until that exists the image is still linked at the start
   of flash, so the limit is measured from there.

   The generated script still describes the whole 512 KB of flash and
   CubeMX owns it, so the limit is checked here instead: the link fails
   when the image ends beyond it.

   .tdata is the last section the generated script loads into flash; the
   image ends where its load image ends. */

IMAGE_LIMIT = 224K;

ASSERT(LOADADDR(.tdata) + SIZEOF(.tdata) <= ORIGIN(FLASH) + IMAGE_LIMIT,
       "image is larger than 224 KB of bank 1 (ADR 0014)")
