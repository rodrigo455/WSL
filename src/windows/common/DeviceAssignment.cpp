/*++

Copyright (c) Microsoft. All rights reserved.

Module Name:

    DeviceAssignment.cpp

Abstract:

    This file contains the implementation for enumerating assignable PCI devices
    and for moving them between the host and the assignable device pool.

    Enumeration is plain configuration manager: devices still owned by the host
    live under the PCI enumerator, and devices that have been dismounted live
    under PCIP. Bind and unbind are the two Msvm_AssignableDeviceService methods
    that Mount-VMHostAssignableDevice and Dismount-VMHostAssignableDevice call,
    plus the devnode disable that Disable-PnpDevice performs.

--*/

#include "precomp.h"
#include "DeviceAssignment.h"
#include <cfgmgr32.h>
#include <setupapi.h>
//
// devpkey.h has no include guard of its own: it defines DEVPKEY_H_INCLUDED unconditionally and then
// re-expands every DEFINE_DEVPROPKEY, which initguid.h (reached through precomp.h) turns into an
// initialized definition rather than a declaration. precomp.h already pulls the header in by way of
// <netioapi.h> -> <ntddndis.h>, so including it again unguarded is a hundred redefinition errors.
//
#ifndef DEVPKEY_H_INCLUDED
#include <devpkey.h>
#endif
#include <WbemIdl.h>
#include "WmiService.h"

#pragma hdrstop

using wsl::windows::common::deviceassignment::AssignableDevice;
using wsl::windows::common::deviceassignment::DeviceState;

namespace {

constexpr auto c_pciEnumerator = L"PCI";
constexpr auto c_dismountedEnumerator = L"PCIP";
constexpr auto c_virtualizationNamespace = L"root\\virtualization\\v2";
constexpr auto c_assignableDeviceService = L"Msvm_AssignableDeviceService";
constexpr auto c_dismountSettingData = L"Msvm_AssignableDeviceDismountSettingData";

// Msvm methods either complete inline or hand back a job to wait on.
constexpr ULONG c_jobStarted = 4096;

// CIM_ConcreteJob.JobState
constexpr ULONG c_jobStateCompleted = 7;

//
// Reads a string or string-list device property verbatim, embedded nulls and all. The two callers
// below disagree about what to do with a list, so the split happens there rather than here.
//
std::wstring GetRawStringProperty(_In_ DEVINST DevInst, _In_ const DEVPROPKEY& Key)
{
    DEVPROPTYPE type{};
    ULONG size{};
    auto result = CM_Get_DevNode_PropertyW(DevInst, &Key, &type, nullptr, &size, 0);
    if (result != CR_BUFFER_SMALL || size == 0)
    {
        return {};
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    result = CM_Get_DevNode_PropertyW(DevInst, &Key, &type, reinterpret_cast<PBYTE>(value.data()), &size, 0);
    if (result != CR_SUCCESS || (type != DEVPROP_TYPE_STRING && type != DEVPROP_TYPE_STRING_LIST))
    {
        return {};
    }

    // Drop the terminator, plus the extra one that ends a list.
    while (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }

    return value;
}

//
// The first entry of the property. For a single string that is the whole value; for a list it is
// the primary entry, which is what matters for a location path or a friendly name.
//
std::wstring GetStringProperty(_In_ DEVINST DevInst, _In_ const DEVPROPKEY& Key)
{
    auto value = GetRawStringProperty(DevInst, Key);
    const auto end = value.find(L'\0');
    if (end != std::wstring::npos)
    {
        value.resize(end);
    }

    return value;
}

//
// Every entry of the property, joined by semicolons. Hardware IDs have to be read this way: the
// first entry is the VEN/DEV/SUBSYS form and the class code only appears in a later one.
//
std::wstring GetStringListProperty(_In_ DEVINST DevInst, _In_ const DEVPROPKEY& Key)
{
    auto value = GetRawStringProperty(DevInst, Key);
    std::replace(value.begin(), value.end(), L'\0', L';');

    return value;
}

//
// A PCI hardware ID carries the class code, for example
// PCI\VEN_10DE&DEV_2D39&CC_030000. Class 06 is a bridge, which cannot be assigned: the point of
// assignment is an endpoint function. Returning the reason rather than a bool keeps --list-devices
// able to explain itself.
//
std::wstring GetIneligibleReason(const AssignableDevice& Device)
{
    // A dismounted device has already passed the host's own eligibility check.
    if (Device.State == DeviceState::Unbound)
    {
        return {};
    }

    const auto& id = Device.HardwareId;
    const auto classCode = id.find(L"&CC_");
    if (classCode != std::wstring::npos && id.compare(classCode + 4, 2, L"06") == 0)
    {
        return L"PCI bridge";
    }

    if (Device.LocationPath.empty())
    {
        return L"no location path";
    }

    return {};
}

std::optional<AssignableDevice> QueryDevice(_In_ PCWSTR DeviceId, _In_ bool Dismounted)
{
    // The ID list includes devnodes that are no longer present, and remounting a device leaves its
    // PCIP node behind as one of those. Locating without CM_LOCATE_DEVNODE_PHANTOM skips them, so a
    // stale dismount is not reported as an unbound device alongside the live PCI node.
    DEVINST devInst{};
    if (CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(DeviceId), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
    {
        return {};
    }

    AssignableDevice device{};
    device.DeviceInstancePath = DeviceId;
    device.LocationPath = GetStringProperty(devInst, DEVPKEY_Device_LocationPaths);
    device.FriendlyName = GetStringProperty(devInst, DEVPKEY_Device_FriendlyName);
    if (device.FriendlyName.empty())
    {
        device.FriendlyName = GetStringProperty(devInst, DEVPKEY_NAME);
    }

    // The whole list, because the class code lives in a later entry than the VEN/DEV one.
    device.HardwareId = GetStringListProperty(devInst, DEVPKEY_Device_HardwareIds);

    if (Dismounted)
    {
        device.State = DeviceState::Unbound;
    }
    else
    {
        ULONG status{};
        ULONG problem{};
        if (CM_Get_DevNode_Status(&status, &problem, devInst, 0) == CR_SUCCESS && WI_IsFlagSet(status, DN_HAS_PROBLEM) && problem == CM_PROB_DISABLED)
        {
            device.State = DeviceState::Disabled;
        }
        else
        {
            device.State = DeviceState::Host;
        }
    }

    device.IneligibleReason = GetIneligibleReason(device);

    return device;
}

std::vector<std::wstring> EnumerateDeviceIds(_In_ PCWSTR Enumerator)
{
    // The list can change between sizing and reading it, so retry on CR_BUFFER_SMALL.
    for (int attempt = 0; attempt < 4; attempt++)
    {
        ULONG size{};
        if (CM_Get_Device_ID_List_SizeW(&size, Enumerator, CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS || size <= 1)
        {
            return {};
        }

        std::wstring buffer(size, L'\0');
        const auto result = CM_Get_Device_ID_ListW(Enumerator, buffer.data(), size, CM_GETIDLIST_FILTER_ENUMERATOR);
        if (result == CR_BUFFER_SMALL)
        {
            continue;
        }
        else if (result != CR_SUCCESS)
        {
            return {};
        }

        // A multi-sz: consecutive null terminated strings ended by an empty one.
        std::vector<std::wstring> ids;
        for (const wchar_t* id = buffer.c_str(); *id != L'\0'; id += wcslen(id) + 1)
        {
            ids.emplace_back(id);
        }

        return ids;
    }

    return {};
}

//
// Devnode disable and enable, which is what Disable-PnpDevice and Enable-PnpDevice do. A device
// has to leave its host driver before Hyper-V will dismount it.
//
void SetDevnodeEnabled(_In_ PCWSTR DeviceInstancePath, _In_ bool Enable)
{
    // wil::unique_hdevinfo is only declared when setupapi.h is seen before wil/resource.h, and the
    // precompiled header includes wil first, so release the list by hand.
    const auto devInfo = SetupDiCreateDeviceInfoListEx(nullptr, nullptr, nullptr, nullptr);
    THROW_LAST_ERROR_IF(devInfo == INVALID_HANDLE_VALUE);

    const auto destroyList = wil::scope_exit([&]() { SetupDiDestroyDeviceInfoList(devInfo); });

    SP_DEVINFO_DATA devInfoData{sizeof(SP_DEVINFO_DATA)};
    THROW_IF_WIN32_BOOL_FALSE(SetupDiOpenDeviceInfoW(devInfo, DeviceInstancePath, nullptr, 0, &devInfoData));

    SP_PROPCHANGE_PARAMS params{};
    params.ClassInstallHeader.cbSize = sizeof(params.ClassInstallHeader);
    params.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    params.StateChange = Enable ? DICS_ENABLE : DICS_DISABLE;
    params.Scope = DICS_FLAG_CONFIGSPECIFIC;
    params.HwProfile = 0;

    THROW_IF_WIN32_BOOL_FALSE(SetupDiSetClassInstallParamsW(devInfo, &devInfoData, &params.ClassInstallHeader, sizeof(params)));

    THROW_IF_WIN32_BOOL_FALSE(SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devInfo, &devInfoData));

    WSL_LOG(
        "DeviceAssignmentSetDevnodeEnabled", TraceLoggingValue(DeviceInstancePath, "device"), TraceLoggingValue(Enable, "enable"));
}

std::wstring GetStringValue(const wil::com_ptr<IWbemClassObject>& Object, _In_ PCWSTR Name)
{
    wil::unique_variant value;
    if (FAILED(Object->Get(Name, 0, &value, nullptr, nullptr)) || value.vt != VT_BSTR)
    {
        return {};
    }

    return value.bstrVal;
}

ULONG GetUlongValue(const wil::com_ptr<IWbemClassObject>& Object, _In_ PCWSTR Name)
{
    wil::unique_variant value;
    if (FAILED(Object->Get(Name, 0, &value, nullptr, nullptr)))
    {
        return 0;
    }

    if (value.vt == VT_I4)
    {
        return static_cast<ULONG>(value.lVal);
    }
    else if (value.vt == VT_BSTR)
    {
        return static_cast<ULONG>(wcstoul(value.bstrVal, nullptr, 10));
    }

    return 0;
}

void PutString(const wil::com_ptr<IWbemClassObject>& Object, _In_ PCWSTR Name, _In_ PCWSTR Value)
{
    wil::unique_variant variant;
    variant.vt = VT_BSTR;
    variant.bstrVal = wil::make_bstr(Value).release();
    THROW_IF_FAILED(Object->Put(Name, 0, &variant, 0));
}

void PutBool(const wil::com_ptr<IWbemClassObject>& Object, _In_ PCWSTR Name, _In_ bool Value)
{
    wil::unique_variant variant;
    variant.vt = VT_BOOL;
    variant.boolVal = Value ? VARIANT_TRUE : VARIANT_FALSE;
    THROW_IF_FAILED(Object->Put(Name, 0, &variant, 0));
}

//
// Waits for a Msvm job to finish. Hyper-V returns 4096 from a method that could not complete
// synchronously, along with a reference to the job object.
//
void WaitForJob(wsl::core::WmiService& Service, const wil::com_ptr<IWbemClassObject>& OutParameters)
{
    const auto returnValue = GetUlongValue(OutParameters, L"ReturnValue");
    if (returnValue == 0)
    {
        return;
    }

    THROW_HR_IF_MSG(HRESULT_FROM_WIN32(returnValue), returnValue != c_jobStarted, "Msvm method failed: %lu", returnValue);

    const auto jobPath = GetStringValue(OutParameters, L"Job");
    THROW_HR_IF_MSG(E_UNEXPECTED, jobPath.empty(), "Msvm method reported a job with no path");

    // The operation involves a PnP stop and an IOMMU reconfiguration, so it can take seconds.
    for (int attempt = 0; attempt < 600; attempt++)
    {
        wil::com_ptr<IWbemClassObject> job;
        THROW_IF_FAILED(Service->GetObject(wil::make_bstr(jobPath.c_str()).get(), 0, nullptr, &job, nullptr));

        const auto state = GetUlongValue(job, L"JobState");
        if (state == c_jobStateCompleted)
        {
            return;
        }
        else if (state > c_jobStateCompleted)
        {
            const auto description = GetStringValue(job, L"ErrorDescription");
            const auto errorCode = GetUlongValue(job, L"ErrorCode");
            WSL_LOG(
                "DeviceAssignmentJobFailed",
                TraceLoggingValue(state, "jobState"),
                TraceLoggingValue(errorCode, "errorCode"),
                TraceLoggingValue(description.c_str(), "errorDescription"));

            THROW_HR_MSG(
                errorCode != 0 ? HRESULT_FROM_WIN32(errorCode) : E_FAIL, "Msvm job failed, state %lu: %ls", state, description.c_str());
        }

        Sleep(100);
    }

    THROW_HR_MSG(HRESULT_FROM_WIN32(ERROR_TIMEOUT), "Msvm job did not complete");
}

//
// Returns the __PATH of the host's single Msvm_AssignableDeviceService instance. Building the key
// by hand would mean guessing the host name and creation class names, so ask for it instead.
//
std::wstring GetAssignableDeviceServicePath(wsl::core::WmiService& Service)
{
    wil::com_ptr<IEnumWbemClassObject> enumerator;
    THROW_IF_FAILED(Service->CreateInstanceEnum(
        wil::make_bstr(c_assignableDeviceService).get(), WBEM_FLAG_RETURN_IMMEDIATELY | WBEM_FLAG_FORWARD_ONLY, nullptr, &enumerator));

    wil::com_ptr<IWbemClassObject> instance;
    ULONG returned{};
    THROW_IF_FAILED(enumerator->Next(WBEM_INFINITE, 1, &instance, &returned));
    THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), returned == 0, "%ls is not present", c_assignableDeviceService);

    auto path = GetStringValue(instance, L"__PATH");
    THROW_HR_IF(E_UNEXPECTED, path.empty());

    return path;
}

//
// Hyper-V takes embedded instances as strings holding the object's WMI text representation, so
// the setting data object has to be serialized before it can be passed as a parameter.
//
std::wstring ToEmbeddedInstance(const wil::com_ptr<IWbemClassObject>& Object)
{
    const auto textSource = wil::CoCreateInstance<WbemObjectTextSrc, IWbemObjectTextSrc>();

    wil::unique_bstr text;
    THROW_IF_FAILED(textSource->GetText(0, Object.get(), WMI_OBJ_TEXT_WMI_DTD_2_0, nullptr, text.put()));

    return text.get();
}

wil::com_ptr<IWbemClassObject> InvokeMethod(
    wsl::core::WmiService& Service, _In_ PCWSTR ObjectPath, _In_ PCWSTR Method, const wil::com_ptr<IWbemClassObject>& Parameters)
{
    wil::com_ptr<IWbemClassObject> outParameters;
    THROW_IF_FAILED_MSG(
        Service->ExecMethod(wil::make_bstr(ObjectPath).get(), wil::make_bstr(Method).get(), 0, nullptr, Parameters.get(), &outParameters, nullptr),
        "%ls failed",
        Method);

    WaitForJob(Service, outParameters);

    return outParameters;
}

wil::com_ptr<IWbemClassObject> SpawnMethodParameters(wsl::core::WmiService& Service, _In_ PCWSTR Class, _In_ PCWSTR Method)
{
    wil::com_ptr<IWbemClassObject> classObject;
    THROW_IF_FAILED(Service->GetObject(wil::make_bstr(Class).get(), WBEM_FLAG_RETURN_WBEM_COMPLETE, nullptr, &classObject, nullptr));

    wil::com_ptr<IWbemClassObject> signature;
    THROW_IF_FAILED(classObject->GetMethod(Method, 0, &signature, nullptr));

    wil::com_ptr<IWbemClassObject> parameters;
    THROW_IF_FAILED(signature->SpawnInstance(0, &parameters));

    return parameters;
}

wil::com_ptr<IWbemClassObject> SpawnInstance(wsl::core::WmiService& Service, _In_ PCWSTR Class)
{
    wil::com_ptr<IWbemClassObject> classObject;
    THROW_IF_FAILED(Service->GetObject(wil::make_bstr(Class).get(), WBEM_FLAG_RETURN_WBEM_COMPLETE, nullptr, &classObject, nullptr));

    wil::com_ptr<IWbemClassObject> instance;
    THROW_IF_FAILED(classObject->SpawnInstance(0, &instance));

    return instance;
}

} // namespace

std::vector<AssignableDevice> wsl::windows::common::deviceassignment::Enumerate(_In_ bool IncludeIneligible)
{
    std::vector<AssignableDevice> devices;
    auto collect = [&](PCWSTR enumerator, bool dismounted) {
        for (const auto& id : EnumerateDeviceIds(enumerator))
        {
            auto device = QueryDevice(id.c_str(), dismounted);
            if (device.has_value() && (IncludeIneligible || device->Eligible()))
            {
                devices.emplace_back(std::move(device.value()));
            }
        }
    };

    collect(c_dismountedEnumerator, true);
    collect(c_pciEnumerator, false);

    return devices;
}

std::optional<AssignableDevice> wsl::windows::common::deviceassignment::Find(_In_ PCWSTR Device)
{
    // Match on either path, and always search every device including the ineligible ones so that
    // naming a bridge produces "PCI bridge" rather than "no such device".
    const std::wstring_view target{Device};
    for (auto& device : Enumerate(true))
    {
        if (wsl::shared::string::IsEqual(device.DeviceInstancePath, target, true) ||
            wsl::shared::string::IsEqual(device.LocationPath, target, true))
        {
            return device;
        }
    }

    return {};
}

std::wstring wsl::windows::common::deviceassignment::Unbind(_In_ PCWSTR Device)
{
    const auto found = Find(Device);
    THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), !found.has_value(), "no such PCI device: %ls", Device);
    THROW_HR_IF_MSG(
        HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED),
        !found->Eligible(),
        "%ls cannot be assigned: %ls",
        Device,
        found->IneligibleReason.c_str());

    // Already dismounted, so there is nothing to do. Returning the path keeps the operation
    // idempotent for a caller that is retrying.
    if (found->State == DeviceState::Unbound)
    {
        return found->DeviceInstancePath;
    }

    WSL_LOG(
        "DeviceAssignmentUnbind",
        TraceLoggingValue(found->DeviceInstancePath.c_str(), "device"),
        TraceLoggingValue(found->LocationPath.c_str(), "locationPath"));

    // The device has to stop being the host's before Hyper-V will hand it out.
    if (found->State != DeviceState::Disabled)
    {
        SetDevnodeEnabled(found->DeviceInstancePath.c_str(), false);
    }

    // If the dismount fails the device is left disabled and useful to nobody, so put it back.
    auto restoreDevnode =
        wil::scope_exit_log(WI_DIAGNOSTICS_INFO, [&]() { SetDevnodeEnabled(found->DeviceInstancePath.c_str(), true); });

    wsl::core::WmiService service{c_virtualizationNamespace};
    const auto servicePath = GetAssignableDeviceServicePath(service);

    auto settingData = SpawnInstance(service, c_dismountSettingData);
    PutString(settingData, L"DeviceInstancePath", found->DeviceInstancePath.c_str());
    PutString(settingData, L"DeviceLocationPath", found->LocationPath.c_str());

    // These two are what Dismount-VMHostAssignableDevice -Force clears. Without -Force the host
    // refuses any device whose chipset cannot guarantee isolation, which on a consumer board is
    // most of them.
    PutBool(settingData, L"RequireAcsSupport", false);
    PutBool(settingData, L"RequireDeviceMitigations", false);

    auto parameters = SpawnMethodParameters(service, c_assignableDeviceService, L"DismountAssignableDevice");
    PutString(parameters, L"DismountSettingData", ToEmbeddedInstance(settingData).c_str());

    const auto outParameters = InvokeMethod(service, servicePath.c_str(), L"DismountAssignableDevice", parameters);
    restoreDevnode.release();

    auto dismountedPath = GetStringValue(outParameters, L"DismountedDeviceInstancePath");
    if (dismountedPath.empty())
    {
        // The dismount succeeded but the out parameter was not populated. Re-reading the pool is
        // authoritative, and the caller needs a path to hand to HCS.
        const auto refreshed = Find(found->LocationPath.c_str());
        THROW_HR_IF(E_UNEXPECTED, !refreshed.has_value() || refreshed->State != DeviceState::Unbound);

        dismountedPath = refreshed->DeviceInstancePath;
    }

    WSL_LOG("DeviceAssignmentUnbindComplete", TraceLoggingValue(dismountedPath.c_str(), "dismountedDevice"));

    return dismountedPath;
}

void wsl::windows::common::deviceassignment::Bind(_In_ PCWSTR Device)
{
    const auto found = Find(Device);
    THROW_HR_IF_MSG(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), !found.has_value(), "no such PCI device: %ls", Device);

    // Nothing to mount, but the devnode may still be disabled from a previous unbind.
    if (found->State != DeviceState::Unbound)
    {
        if (found->State == DeviceState::Disabled)
        {
            SetDevnodeEnabled(found->DeviceInstancePath.c_str(), true);
        }

        return;
    }

    WSL_LOG(
        "DeviceAssignmentBind",
        TraceLoggingValue(found->DeviceInstancePath.c_str(), "device"),
        TraceLoggingValue(found->LocationPath.c_str(), "locationPath"));

    wsl::core::WmiService service{c_virtualizationNamespace};
    const auto servicePath = GetAssignableDeviceServicePath(service);

    auto parameters = SpawnMethodParameters(service, c_assignableDeviceService, L"MountAssignableDevice");
    PutString(parameters, L"DeviceInstancePath", found->DeviceInstancePath.c_str());
    PutString(parameters, L"DeviceLocationPath", found->LocationPath.c_str());

    const auto outParameters = InvokeMethod(service, servicePath.c_str(), L"MountAssignableDevice", parameters);

    // The mount recreates the devnode under PCI, and it comes back disabled because that is how
    // it was left. Re-enable it so the host driver can claim the device again.
    auto mountedPath = GetStringValue(outParameters, L"MountedDeviceInstancePath");
    if (mountedPath.empty())
    {
        const auto refreshed = Find(found->LocationPath.c_str());
        if (!refreshed.has_value())
        {
            // The device came back somewhere this enumeration cannot see it. A reboot fixes that,
            // and failing here would not.
            WSL_LOG("DeviceAssignmentBindNoDevnode", TraceLoggingValue(found->LocationPath.c_str(), "locationPath"));
            return;
        }

        mountedPath = refreshed->DeviceInstancePath;
    }

    // A failure to re-enable leaves the device present but stopped, which a reboot or a manual
    // Enable-PnpDevice fixes. It does not make the mount any less done.
    try
    {
        SetDevnodeEnabled(mountedPath.c_str(), true);
    }
    CATCH_LOG()

    WSL_LOG("DeviceAssignmentBindComplete", TraceLoggingValue(mountedPath.c_str(), "mountedDevice"));
}
