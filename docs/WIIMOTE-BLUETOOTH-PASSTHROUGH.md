# Wii Remote por Bluetooth passthrough (adaptador USB dedicado)

Código portado de [GamesExObscura/CemuX](https://github.com/GamesExObscura/CemuX) (MPL-2.0).
Solo Windows x64.

## Qué aporta

- Wii Remote, MotionPlus y Nunchuk/Classic con hotplug: al encender o apagar un mando el juego
  recibe la conexión/desconexión real (callbacks WPAD), el tipo de extensión y el nivel de batería.
- Asignación automática: un mando nuevo ocupa la primera posición libre (controlador 2..8) sin
  pisar los perfiles existentes. Opcional: "Automatically configure connected controllers" en
  Input Settings alterna Pro/Wiimote según lo que haya conectado.
- Altavoz del mando (WPADControlSpeaker / WPADSendStreamData) y volumen configurable.
- Passthrough Bluetooth: Cemu habla HCI directamente con un adaptador USB dedicado (WinUSB vía
  libusb), igual que haría la consola. Emparejamiento con el botón rojo SYNC, reconexión con A,
  enlaces guardados entre sesiones, apagado del mando gestionado.

Sin passthrough todo lo anterior funciona también con un Wii Remote emparejado al Bluetooth
de Windows (hidapi), pero el passthrough evita los problemas del stack Bluetooth de Windows.

## Configuración del adaptador

1. Conectar un adaptador USB Bluetooth **clásico** (BR/EDR) que vaya a quedar dedicado a Cemu.
   CemuX solo tiene probado el ASUS USB-BT600 (VID:PID 0b05:1d70, el valor por defecto).
2. Mirar su VID:PID en el Administrador de dispositivos (Propiedades > Detalles > Id. de hardware).
3. Con [Zadig](https://zadig.akeo.ie/) instalar el driver **WinUSB** solo para ese adaptador
   (no para el Bluetooth que uses normalmente).
4. En Cemu: Options > Input settings:
   - "Adapter USB ID..." → escribir el VID:PID (formato `0b05:1d70`).
   - Marcar "USB Bluetooth passthrough for Wii Remotes".
5. Pulsar SYNC (botón rojo bajo la tapa de las pilas) en cada Wii Remote nuevo. Los ya
   emparejados se reconectan pulsando A. Las posiciones de controlador se asignan solas.
6. Si un juego pide "pulsa HOME para añadir un Wii Remote" (Mario Party 10, por ejemplo), pulsar HOME
   en el propio Wii Remote: Cemu no emula el menú HOME, así que esa pulsación se traduce en un
   evento de desconexión y reconexión del mando, que es lo que el juego espera para añadirlo.

Para el altavoz con DolphinBar/HID o adaptador USB, activar "Speaker compatibility (3 kHz)" en
los ajustes del Wiimote antes de arrancar el juego (reduce la calidad del audio del mando).

## Dónde está el código

- `src/input/api/Wiimote/` — proveedor reescrito (hotplug, MotionPlus, altavoz, batería) y
  `usb/` (host HCI/L2CAP sobre libusb, enlaces guardados cifrados con DPAPI).
- `src/Cafe/OS/libs/padscore/` — implementación de WPAD/KPAD: callbacks de conexión y extensión,
  MotionPlus (KPADEnableMpls y compañía, `MplsProcessing.h`), altavoz, sync, batería.
- `src/input/InputManager.*` — asignación automática de Wii Remotes y modo opcional Pro/Wiimote.
- `src/gui/wxgui/input/` — casillas de passthrough, ID del adaptador, volumen y compatibilidad.
- `src/config/CemuConfig.*` — claves `wiimote_usb_bluetooth`, `wiimote_usb_adapter`,
  `automatic_controller_setup`.

Diferencias respecto a CemuX: no se han traído ni la guitarra virtual, ni los procesos auxiliares
del menú HOME, ni las pruebas automáticas; los hilos del proveedor Wiimote solo arrancan si un
perfil usa la API Wiimote o si el passthrough está activado (comportamiento de upstream).
