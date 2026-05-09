# Zephyr-CP2112-emulator

Convert any USB-enabled Zephyr-supported board into a Linux-native GPIO and I2C controller.

When plugged into Linux, the in-tree `hid-cp2112` driver binds to it and exposes:
- an I2C adapter (`/dev/i2c-N`)
- a GPIO chip (8 lines)

## Requirements

- Zephyr with USB Device Next (`usbd`)
- HID device support
- One I2C controller
- Exactly 8 GPIOs

## Devicetree

Provide a node:

- `compatible = "zephyr,cp2112-emul"`
- `i2c-bus = <&...>`
- `gpios = <8 entries>`

Also define a HID device node (`cp2112_hid`) under the UDC.

## Adding a new board

To port this to a new board, add a small devicetree overlay.

- Enable an I2C controller and point `i2c-bus` to it  
- Provide exactly 8 GPIOs  
- Enable the USB device controller (`zephyr_udc0`)  
- Add a `zephyr,hid-device` node (`cp2112_hid`) with 64-byte reports  

See [boards/blackpill_f411ce.overlay](boards/blackpill_f411ce.overlay) for a complete example.

## Tested Boards
- [WeAct Blackpill F411CE](boards/blackpill_f411ce.overlay)
- [Raspberry Pi Pico RP2040](boards/rpi_pico_rp2040.overlay)

## Behavior

- I2C transfers use Zephyr’s I2C API  
- Single transfer in flight  
- GPIOs map directly to DT pins  

## Limitations

- Partial protocol implementation  
- SMBus settings are not enforced  
- Cancel does not abort an active transfer  

## License

Apache-2.0