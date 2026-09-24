/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DeviceAssignment.h

Abstract:

    This file contains declarations for enumerating PCI devices that can be
    assigned to a utility VM, and for moving them between the host and the
    pool of devices available for assignment.

--*/

#pragma once

namespace wsl::windows::common::deviceassignment {

enum class DeviceState
{
    // Bound to a Windows driver.
    Host,

    // The devnode is disabled, but the device has not been dismounted.
    Disabled,

    // Dismounted from the host and available for assignment.
    Unbound,
};

struct AssignableDevice
{
    // The PCIP\VEN_... path of a dismounted device, or the PCI\VEN_... path of a device still
    // owned by the host. This is the string the HCS DeviceInstancePath field wants.
    std::wstring DeviceInstancePath;

    // PCIROOT(0)#PCI(0100)#PCI(0000). Stable across a dismount, which is why it is the more
    // useful handle for a user.
    std::wstring LocationPath;

    std::wstring FriendlyName;
    std::wstring HardwareId;
    DeviceState State{DeviceState::Host};

    // Empty when the device can be assigned. Otherwise a short explanation of why not.
    std::wstring IneligibleReason;

    bool Eligible() const noexcept
    {
        return IneligibleReason.empty();
    }
};

//
// Enumerates PCI devices on the host and devices already dismounted for assignment.
//
// N.B. IncludeIneligible adds devices that cannot be assigned, each carrying the reason.
//
std::vector<AssignableDevice> Enumerate(_In_ bool IncludeIneligible = false);

//
// Looks up a single device by either a location path or a device instance path.
//
std::optional<AssignableDevice> Find(_In_ PCWSTR Device);

//
// Disables the devnode and dismounts the device from the host, returning the device instance
// path of the dismounted device. This is the string to hand to HCS.
//
// N.B. Requires administrator privileges.
//
std::wstring Unbind(_In_ PCWSTR Device);

//
// Mounts a dismounted device back onto the host and re-enables its devnode.
//
// N.B. Requires administrator privileges.
//
void Bind(_In_ PCWSTR Device);

} // namespace wsl::windows::common::deviceassignment
