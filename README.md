# Logitech K780 Fn Lock for Windows

A lightweight Windows utility for changing the top-row behavior of the **Logitech K780** keyboard when connected through a **Logitech Unifying USB receiver**. It allows the keyboard to use F1–F12 directly without Logitech Options.


## Usage

```cmd
k780fn.exe status
```

Show the current Fn-key mode.

```cmd
k780fn.exe fkeys
```

Fn Lock on.

```cmd
k780fn.exe media
```

Fn Lock off (default mode).

---

K780-FnLock.exe is a windowless version without any arguments needed.

When launched, it automatically:

1. Detects the Logitech Unifying receiver.
2. Locates the K780 in the receiver pairing table.
3. Enables Fn Lock.
4. Exits.

## Availability

Tested on Windows 11.

Bluetooth connections are not supported.


## Notes

The Fn-key setting will be reset after reboot, reconnecting the receiver, or reinitializing the keyboard. 
