# raspberry-usb-emulator

USB Raw Gadget emulator for Raspberry Pi 4.

```bash
make
sudo modprobe raw_gadget
sudo ./usb_emulator --def device.def --driver dwc2 --udc fe980000.usb --speed full
```

Create device.def:

```bash
python3 tools/make_device_def.py ../usb-config-generator/output/usb3_vendor_bulk/case.json -o device.def
```
