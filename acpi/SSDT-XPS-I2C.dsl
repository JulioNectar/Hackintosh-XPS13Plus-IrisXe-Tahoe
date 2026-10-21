/*
 * SSDT-XPS-I2C (variante GPIO): o touchscreen (TPL0) e o touchpad (TPD0) usam os recursos originais do firmware,
 * com GpioInt no \_SB.GPI0 (atendido pelo VoodooGPIOTigerLakeLP), em vez de só o barramento I2C (polling).
 * O _INI e o _CRS originais foram renomeados para XINI/XCRS no config.plist.
 */
DefinitionBlock ("", "SSDT", 2, "HACK", "XPSI2C", 0x00002000)
{
    External (_SB_.PC00.I2C0, DeviceObj)
    External (_SB_.PC00.I2C1, DeviceObj)
    External (_SB_.PC00.I2C0.TPL0, DeviceObj)
    External (_SB_.PC00.I2C1.TPD0, DeviceObj)
    External (_SB_.PC00.I2C0.TPL0.XCRS, MethodObj)
    External (_SB_.PC00.I2C0.TPL0.XINI, MethodObj)
    External (_SB_.PC00.I2C1.TPD0.XCRS, MethodObj)
    External (_SB_.PC00.I2C1.TPD0.XINI, MethodObj)

    Scope (_SB.PC00.I2C0)
    {
        Name (_ADR, 0x00150000)
    }

    Scope (_SB.PC00.I2C1)
    {
        Name (_ADR, 0x00150001)
    }

    Scope (_SB.PC00.I2C0.TPL0)
    {
        Method (_INI, 0, NotSerialized)
        {
            XINI ()
        }

        Method (_CRS, 0, NotSerialized)
        {
            Return (XCRS ())
        }
    }

    Scope (_SB.PC00.I2C1.TPD0)
    {
        Method (_INI, 0, NotSerialized)
        {
            XINI ()
        }

        Method (_CRS, 0, NotSerialized)
        {
            Return (XCRS ())
        }
    }
}
