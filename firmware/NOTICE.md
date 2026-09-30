# The RX-888 firmware in this module

`SDDC_FX3.img` is the FX3 firmware of ringof/rx888-firmware release 0.1.0,
unmodified: <https://github.com/ringof/rx888-firmware>, SHA-256
f1c682293c5cb1714b75e8b8cfad0e6cfe86b5f83b987082d425404dc56e4a06. The same
image is ka9q-radio's `W1EUJ_0.1.0_FX3.img`.

The module loads it into the RX-888's FX3 controller, a separate processor;
the module's own code does not link it. Its application code is under the MIT
licence below. It is built with the Cypress (now Infineon) EZ-USB FX3 SDK,
whose libraries and the ThreadX kernel inside them are under the Cypress
Software License Agreement, also below. Its section 1.3 grants a licence to
"reproduce, sublicense and distribute the Firmware [...] in object code form
only, with the applicable Licensee Product", a product that incorporates a
Cypress integrated circuit (section 1.1). This module carries the image apart
from any board; an operator who wants no copy of it here can leave it out and
name their own with `module.firmware`. This release of
the firmware leaves out the GPL-licensed R82xx tuner driver of earlier SDDC
firmware, which could not be combined with the SDK's licence.

`module.firmware` loads a different image instead.
