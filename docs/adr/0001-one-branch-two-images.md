---
status: accepted
---

# One branch, two images

The TIM runs as either the VCU or the BMS master controller, and the two roles share most of their code (shell, logger, CAN path, SPI3 bus lock). We build both the VCU image and the BMS image from one branch as two CMake targets, with common code in `App/common/`, image-only code in `App/vcu/` and `App/bms/`, and one CubeMX project per image under `targets/vcu/` and `targets/bms/`. A change to common code that breaks either image then fails the same build, which a branch per image can never guarantee.

## Considered options

- **A branch per image with an enforced sync.** Rejected: drift is the default state, every common fix needs a merge or cherry-pick, and no single commit can prove both images build.
- **One CubeMX project with a superset `.ioc`.** Rejected: the BMS master reuses pins the VCU drives differently (PC14 lands on an opto output, GPIO_1 is the BQ79600 chip select), so one pin configuration cannot be safe on both boards.
- **One shared copy of `Drivers/` and `Middlewares/`.** Rejected: CubeMX owns those folders per project, and generated code is never hand-edited. Each project keeps its own copy.
- **A third bring-up image for a bare TIM.** Rejected: the VCU image is used for bare-TIM bring-up, so there are exactly two images.

## Consequences

- Common code never checks which image it is built into: no `#ifdef` on the image. Image-specific behaviour lives in that image's folder and reaches common code through hooks (for example a shell command registration call). A file that needs an image switch belongs in `App/vcu/` or `App/bms/`.
- "All task priorities and stack sizes in one place" becomes one `tasks.h` per image.
- Because the HAL and FreeRTOS are duplicated, the build fails if the two `.ioc` files name different firmware package versions.
- The BMS image never links the IMU driver and holds the IMU chip select inactive, so it runs whether or not the IMU is fitted. Not fitting the part is the hardware fallback if the shared SPI3 MISO line causes trouble.
- One CMake preset builds both images. A GitHub Actions job runs the same preset on every push and pull request to `dev`; CubeMX never runs in CI because generated code is committed.
