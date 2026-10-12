//
//  SimpleMtkWlan.cpp
//  SimpleMtkWlan
//
//  Created by laobamac (王孝慈) on 2026/3/18.
//  Copyright © 2026 laobamac (王孝慈). All rights reserved.
//

/*
* Copyright (C) 2020  钟先耀
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/
#include "SimpleMtkWlan.hpp"
#include "MtkLog.hpp"
#include <linux/types.h>
#include <linux/kernel.h>

#include <IOKit/IOInterruptController.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/network/IONetworkMedium.h>
#include <net/ethernet.h>
#include <crypto/sha1.h>
#include <net80211/ieee80211_node.h>
#include <net80211/ieee80211_ioctl.h>

#define super IOEthernetController
OSDefineMetaClassAndStructors(SimpleMtkWlan, IOEthernetController)
OSDefineMetaClassAndStructors(itlwm, SimpleMtkWlan)
OSDefineMetaClassAndStructors(CTimeout, OSObject)

IOWorkLoop *_fWorkloop;
IOCommandGate *_fCommandGate;

bool SimpleMtkWlan::init(OSDictionary *properties)
{
    return super::init(properties);
}

#define  PCI_MSI_FLAGS        2    /* Message Control */
#define  PCI_CAP_ID_MSI        0x05    /* Message Signalled Interrupts */
#define  PCI_MSIX_FLAGS        2    /* Message Control */
#define  PCI_CAP_ID_MSIX    0x11    /* MSI-X */
#define  PCI_MSIX_FLAGS_ENABLE    0x8000    /* MSI-X enable */
#define  PCI_MSI_FLAGS_ENABLE    0x0001    /* MSI feature enabled */

static void pciMsiSetEnable(IOPCIDevice *device, UInt8 msiCap, int enable)
{
    u16 control;
    
    control = device->configRead16(msiCap + PCI_MSI_FLAGS);
    control &= ~PCI_MSI_FLAGS_ENABLE;
    if (enable)
        control |= PCI_MSI_FLAGS_ENABLE;
    device->configWrite16(msiCap + PCI_MSI_FLAGS, control);
}

static void pciMsiXClearAndSet(IOPCIDevice *device, UInt8 msixCap, UInt16 clear, UInt16 set)
{
    u16 ctrl;
    
    ctrl = device->configRead16(msixCap + PCI_MSIX_FLAGS);
    ctrl &= ~clear;
    ctrl |= set;
    device->configWrite16(msixCap + PCI_MSIX_FLAGS, ctrl);
}

// Publishes the driver log on the PCI device, where it survives a failed start and
// check.sh can read it without the unified log (unavailable in the installer).
void SimpleMtkWlan::publishLog(IOService *target)
{
    if (!target)
        return;
    const size_t max = 65536;
    char *buf = (char *)IOMalloc(max);
    if (!buf)
        return;
    mtk_log_snapshot(buf, max);
    OSString *str = OSString::withCString(buf);
    if (str) {
        target->setProperty("SimpleMtkWlanLog", str);
        str->release();
    }
    IOFree(buf, max);
}

void SimpleMtkWlan::recordAttachStage(IOService *provider, const char *stage,
                                    IOReturn result)
{
    setProperty("SimpleMtkWlanStage", stage);
    setProperty("SimpleMtkWlanError", result, 32);
    setProperty("SimpleMtkWlanVersion", SMTW_VERSION);
    if (provider) {
        // Keep the last stage on the PCI device even if start() fails and this
        // controller is released before it can publish its user client.
        provider->setProperty("SimpleMtkWlanStage", stage);
        provider->setProperty("SimpleMtkWlanError", result, 32);
        provider->setProperty("SimpleMtkWlanVersion", SMTW_VERSION);
    }
    mtk_log("SimpleMtkWlan %s: stage=%s result=0x%x\n", SMTW_VERSION, stage, result);
    publishLog(provider);
}

IOService* SimpleMtkWlan::probe(IOService *provider, SInt32 *score)
{
    bool isMatch = false;
    char off[4];
    recordAttachStage(provider, "probe");
    if (PE_parse_boot_argn("-mtkwlanoff", off, sizeof(off))) {
        recordAttachStage(provider, "disabled-by-boot-arg", kIOReturnNotPermitted);
        return NULL;
    }
    if (!super::probe(provider, score)) {
        recordAttachStage(provider, "super-probe-failed", kIOReturnError);
        return NULL;
    }
    UInt8 msiCap = 0;
    UInt8 msixCap = 0;
    IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
    if (!device) {
        recordAttachStage(provider, "not-a-pci-device", kIOReturnBadArgument);
        return NULL;
    }
    if (MtkMwx::mwx_match(device)) {
        isMatch = true;
        fHalService = new MtkMwx;
        if (!fHalService) {
            recordAttachStage(provider, "hal-allocation-failed", kIOReturnNoMemory);
            return NULL;
        }
    }
    if (isMatch) {
        device->findPCICapability(PCI_CAP_ID_MSIX, &msixCap);
        if (msixCap) {
            pciMsiXClearAndSet(device, msixCap, PCI_MSIX_FLAGS_ENABLE, 0);
        }
        device->findPCICapability(PCI_CAP_ID_MSI, &msiCap);
        if (msiCap) {
            pciMsiSetEnable(device, msiCap, 1);
        }
        if (!msiCap && !msixCap) {
            recordAttachStage(provider, "no-msi-capability", kIOReturnUnsupported);
            fHalService->release();
            fHalService = NULL;
            return NULL;
        }
        recordAttachStage(provider, "pci-matched");
        return this;
    }
    recordAttachStage(provider, "unsupported-pci-id", kIOReturnUnsupported);
    return NULL;
}

bool SimpleMtkWlan::configureInterface(IONetworkInterface *netif)
{
    IONetworkData *nd;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    
    if (super::configureInterface(netif) == false) {
        XYLog("super failed\n");
        return false;
    }
    
    nd = netif->getParameter(kIONetworkStatsKey);
    if (!nd || !(fpNetStats = (IONetworkStats *)nd->getBuffer())) {
        XYLog("network statistics buffer unavailable?\n");
        return false;
    }
    ifp->netStat = fpNetStats;
    ether_ifattach(ifp, OSDynamicCast(IOEthernetInterface, netif));
    fpNetStats->collisions = 0;
#ifdef __PRIVATE_SPI__
    netif->configureOutputPullModel(fHalService->getDriverInfo()->getTxQueueSize(), 0, 0, IOEthernetInterface::kOutputPacketSchedulingModelNormal, 0);
#endif
    
    return true;
}

struct _ifnet *SimpleMtkWlan::getIfp()
{
    return &fHalService->get80211Controller()->ic_ac.ac_if;
}

IOEthernetInterface *SimpleMtkWlan::getNetworkInterface()
{
    return getIfp()->iface;
}

bool SimpleMtkWlan::createMediumTables(const IONetworkMedium **primary)
{
    IONetworkMedium    *medium;
    
    OSDictionary *mediumDict = OSDictionary::withCapacity(1);
    if (mediumDict == NULL) {
        XYLog("Cannot allocate OSDictionary\n");
        return false;
    }
    
    medium = IONetworkMedium::medium(kIOMediumEthernetAuto, 100 * 1000000);
    IONetworkMedium::addMedium(mediumDict, medium);
    medium->release();
    if (primary) {
        *primary = medium;
    }
    
    bool result = publishMediumDictionary(mediumDict);
    if (!result) {
        XYLog("Cannot publish medium dictionary!\n");
    }

    mediumDict->release();
    return result;
}

void SimpleMtkWlan::joinSSID(const char *ssid_name, const char *ssid_pwd)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    
    if (strlen(ssid_pwd) == 0) {
        memset(&nwkey, 0, sizeof(ieee80211_nwkey));
        nwkey.i_wepon = IEEE80211_NWKEY_OPEN;
        nwkey.i_defkid = 0;
        memcpy(join.i_nwid, ssid_name, strlen(ssid_name));
        join.i_len = strlen(ssid_name);
        join.i_flags = IEEE80211_JOIN_NWKEY;
    } else {
        memset(&wpa, 0, sizeof(ieee80211_wpaparams));
        wpa.i_enabled = 1;
        wpa.i_ciphers = 0;
        wpa.i_groupcipher = 0;
        wpa.i_protos = IEEE80211_WPA_PROTO_WPA1 | IEEE80211_WPA_PROTO_WPA2;
        wpa.i_akms = IEEE80211_WPA_AKM_PSK | IEEE80211_WPA_AKM_8021X | IEEE80211_WPA_AKM_SHA256_PSK | IEEE80211_WPA_AKM_SHA256_8021X;
        memcpy(wpa.i_name, "smtkw", strlen("smtkw"));
        memset(&psk, 0, sizeof(ieee80211_wpapsk));
        memcpy(psk.i_name, "smtkw", strlen("smtkw"));
        psk.i_enabled = 1;
        pbkdf2_sha1(ssid_pwd, (const uint8_t*)ssid_name, strlen(ssid_name),
                    4096, psk.i_psk , 32);
        memset(&nwkey, 0, sizeof(ieee80211_nwkey));
        nwkey.i_wepon = 0;
        nwkey.i_defkid = 0;
        memset(&join, 0, sizeof(ieee80211_join));
        join.i_wpaparams = wpa;
        join.i_wpapsk = psk;
        join.i_flags = IEEE80211_JOIN_WPAPSK | IEEE80211_JOIN_ANY | IEEE80211_JOIN_WPA | IEEE80211_JOIN_8021X;
        join.i_nwkey = nwkey;
        join.i_len = strlen(ssid_name);
        memcpy(join.i_nwid, ssid_name, join.i_len);
    }
    if (ieee80211_add_ess(ic, &join) == 0)
        ic->ic_flags |= IEEE80211_F_AUTO_JOIN;
}

void SimpleMtkWlan::associateSSID(const char *ssid, const char *pwd)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    // Log the next scans in full so the report shows why the join did or did not happen.
    mtk_verbose_scans = 6;
    mtk_log("SimpleMtkWlan: join request ssid_len=%zu password_len=%zu state=%d\n",
            strlen(ssid), strlen(pwd), ic->ic_state);
    if (strlen(pwd) == 0) {
        memcpy(nwid.i_nwid, ssid, 32);
        nwid.i_len = strlen((char *)nwid.i_nwid);
        memset(ic->ic_des_essid, 0, IEEE80211_NWID_LEN);
        ic->ic_des_esslen = nwid.i_len;
        memcpy(ic->ic_des_essid, nwid.i_nwid, nwid.i_len);
        if (ic->ic_des_esslen > 0) {
            /* 'nwid' disables auto-join magic */
            ic->ic_flags &= ~IEEE80211_F_AUTO_JOIN;
        } else if (!TAILQ_EMPTY(&ic->ic_ess)) {
            /* '-nwid' re-enables auto-join */
            ic->ic_flags |= IEEE80211_F_AUTO_JOIN;
        }
        /* disable WPA/WEP */
        ieee80211_disable_rsn(ic);
        ieee80211_disable_wep(ic);
    } else {
        memset(&psk, 0, sizeof(psk));
        memcpy(nwid.i_nwid, ssid, 32);
        nwid.i_len = strlen((char *)nwid.i_nwid);
        memset(ic->ic_des_essid, 0, IEEE80211_NWID_LEN);
        ic->ic_des_esslen = nwid.i_len;
        memcpy(ic->ic_des_essid, nwid.i_nwid, nwid.i_len);
        if (ic->ic_des_esslen > 0) {
            /* 'nwid' disables auto-join magic */
            ic->ic_flags &= ~IEEE80211_F_AUTO_JOIN;
        } else if (!TAILQ_EMPTY(&ic->ic_ess)) {
            /* '-nwid' re-enables auto-join */
            ic->ic_flags |= IEEE80211_F_AUTO_JOIN;
        }
        /* disable WPA/WEP */
        ieee80211_disable_rsn(ic);
        ieee80211_disable_wep(ic);
        size_t passlen = strlen(pwd);
        /* Parse a WPA passphrase */
        if (passlen < 8 || passlen > 63)
            XYLog("wpakey: passphrase must be between "
                  "8 and 63 characters");
        if (nwid.i_len == 0)
            XYLog("wpakey: nwid not set");
        pbkdf2_sha1(pwd, (const uint8_t*)ssid, nwid.i_len, 4096,
                    psk.i_psk, 32);
        psk.i_enabled = 1;
        if (psk.i_enabled) {
            ic->ic_flags |= IEEE80211_F_PSK;
            memcpy(ic->ic_psk, psk.i_psk, sizeof(ic->ic_psk));
            if (ic->ic_flags & IEEE80211_F_WEPON)
                ieee80211_disable_wep(ic);
        } else {
            ic->ic_flags &= ~IEEE80211_F_PSK;
            memset(ic->ic_psk, 0, sizeof(ic->ic_psk));
        }
        memset(&wpa, 0, sizeof(wpa));
        ieee80211_ioctl_getwpaparms(ic, &wpa);
        wpa.i_enabled = psk.i_enabled;
        wpa.i_ciphers = 0;
        wpa.i_groupcipher = 0;
        wpa.i_protos = IEEE80211_WPA_PROTO_WPA1 | IEEE80211_WPA_PROTO_WPA2;
        wpa.i_akms = IEEE80211_WPA_AKM_PSK | IEEE80211_WPA_AKM_8021X | IEEE80211_WPA_AKM_SHA256_PSK | IEEE80211_WPA_AKM_SHA256_8021X;
        ieee80211_ioctl_setwpaparms(ic, &wpa);
    }
    if (ic->ic_state > IEEE80211_S_AUTH && ic->ic_bss != NULL)
        IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_DEAUTH, IEEE80211_REASON_AUTH_LEAVE);
    ieee80211_del_ess(ic, NULL, 0, 1);
    struct ieee80211_node *selbs = ieee80211_node_choose_bss(ic, 0, NULL);
    if (selbs == NULL) {
        if (ic->ic_state != IEEE80211_S_SCAN) {
            ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
        }
    } else {
        if (ic->ic_state > IEEE80211_S_AUTH) {
            ieee80211_node_join_bss(ic, selbs, 1);
            fHalService->getDriverController()->clearScanningFlags();
        } else {
            if (ic->ic_state != IEEE80211_S_SCAN) {
                ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
            }
        }
    }
}

bool SimpleMtkWlan::start(IOService *provider)
{
    int boot_value = 0;
    recordAttachStage(provider, "start");
    if (!super::start(provider)) {
        recordAttachStage(provider, "super-start-failed", kIOReturnError);
        return false;
    }
    pciNub = OSDynamicCast(IOPCIDevice, provider);
    if (!pciNub) {
        recordAttachStage(provider, "not-a-pci-device", kIOReturnBadArgument);
        return false;
    }
    pciNub->setBusMasterEnable(true);
    pciNub->setIOEnable(true);
    pciNub->setMemoryEnable(true);
    pciNub->configWrite8(0x41, 0);
    IOReturn powerResult = pciNub->requestPowerDomainState(kIOPMPowerOn,
        (IOPowerConnection *)getParentEntry(gIOPowerPlane), IOPMLowestState);
    if (powerResult != IOPMNoErr) {
        recordAttachStage(provider, "power-domain-failed", powerResult);
        super::stop(provider);
        return false;
    }
    if (initPCIPowerManagment(pciNub) == false) {
        recordAttachStage(provider, "pci-power-failed", kIOReturnError);
        super::stop(pciNub);
        return false;
    }
    if (_fWorkloop == NULL) {
        recordAttachStage(provider, "workloop-allocation-failed", kIOReturnNoMemory);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    _fCommandGate = IOCommandGate::commandGate(this, (IOCommandGate::Action)SimpleMtkWlan::tsleepHandler);
    if (_fCommandGate == 0) {
        recordAttachStage(provider, "command-gate-allocation-failed", kIOReturnNoMemory);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    IOReturn gateResult = _fWorkloop->addEventSource(_fCommandGate);
    if (gateResult != kIOReturnSuccess) {
        recordAttachStage(provider, "command-gate-add-failed", gateResult);
        super::stop(provider);
        releaseAll();
        return false;
    }
    const IONetworkMedium *primaryMedium;
    if (!createMediumTables(&primaryMedium) ||
        !setCurrentMedium(primaryMedium) || !setSelectedMedium(primaryMedium)) {
        recordAttachStage(provider, "network-medium-failed", kIOReturnError);
        super::stop(provider);
        releaseAll();
        return false;
    }
    if (!fHalService->initWithController(this, _fWorkloop, _fCommandGate)) {
        recordAttachStage(provider, "hal-init-failed", kIOReturnError);
        super::stop(provider);
        releaseAll();
        return false;
    }
    
    if (PE_parse_boot_argn("-novht", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOVHT;
    if (PE_parse_boot_argn("-noht40", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOHT40;
    
    recordAttachStage(provider, "firmware-and-hardware-attach");
    if (!fHalService->attach(pciNub)) {
        recordAttachStage(provider, "firmware-or-hardware-attach-failed", kIOReturnError);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    // The MAC address comes from the firmware during hardware init (mwx_preinit),
    // whose failure attach() does not report. Without it the interface cannot exist.
    if (IEEE80211_ADDR_EQ(etheranyaddr, fHalService->get80211Controller()->ic_myaddr)) {
        recordAttachStage(provider, "hardware-init-failed", kIOReturnNotReady);
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    if (!attachInterface((IONetworkInterface **)&fNetIf, true)) {
        recordAttachStage(provider, "ethernet-interface-failed", kIOReturnError);
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    fWatchdogWorkLoop = IOWorkLoop::workLoop();
    if (fWatchdogWorkLoop == NULL) {
        recordAttachStage(provider, "watchdog-workloop-failed", kIOReturnNoMemory);
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    watchdogTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &SimpleMtkWlan::watchdogAction));
    if (!watchdogTimer) {
        recordAttachStage(provider, "watchdog-timer-failed", kIOReturnNoMemory);
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    IOReturn timerResult = fWatchdogWorkLoop->addEventSource(watchdogTimer);
    if (timerResult != kIOReturnSuccess) {
        recordAttachStage(provider, "watchdog-add-failed", timerResult);
        fHalService->detach(pciNub);
        super::stop(provider);
        releaseAll();
        return false;
    }
    setLinkStatus(kIONetworkLinkValid);
    OSObject *wifiEntryObject = NULL;
    OSDictionary *wifiEntry = NULL;
    OSString *entryKey = NULL;
    OSDictionary *wifiDict = OSDynamicCast(OSDictionary, getProperty("WiFiConfig"));
    if (wifiDict != NULL) {
        OSCollectionIterator *iterator = OSCollectionIterator::withCollection(wifiDict);
        while ((wifiEntryObject = iterator->getNextObject())) {
            entryKey = OSDynamicCast(OSString, wifiEntryObject);
            if (entryKey == NULL) {
                continue;
            }
            wifiEntry = OSDynamicCast(OSDictionary, wifiDict->getObject(entryKey));
            if (wifiEntry == NULL) {
                continue;
            }
            OSString *ssidObj = OSDynamicCast(OSString, wifiEntry->getObject("ssid"));
            OSString *pwdObj = OSDynamicCast(OSString, wifiEntry->getObject("password"));
            if (ssidObj == NULL || pwdObj == NULL || ssidObj->isEqualTo("")) {
                continue;
            }
            
            joinSSID(ssidObj->getCStringNoCopy(), pwdObj->getCStringNoCopy());
        }
        iterator->release();
    }
    if (TAILQ_EMPTY(&fHalService->get80211Controller()->ic_ess)) {
        fHalService->get80211Controller()->ic_flags |= IEEE80211_F_AUTO_JOIN;
    }
    registerService();
    fNetIf->registerService();
    recordAttachStage(provider, "registered");
    return true;
}

void SimpleMtkWlan::watchdogAction(IOTimerEventSource *timer)
{
    static uint32_t logPublishTick;
    if ((++logPublishTick % 5) == 0)
        publishLog(pciNub);
    struct _ifnet *ifp = getIfp();
    (*ifp->if_watchdog)(ifp);
    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
}

const OSString * SimpleMtkWlan::newVendorString() const
{
    return OSString::withCString("Apple");
}

const OSString * SimpleMtkWlan::newModelString() const
{
    return OSString::withCString("MediaTek MT792x Wireless Card");
}

bool SimpleMtkWlan::initPCIPowerManagment(IOPCIDevice *provider)
{
    UInt16 reg16;

    reg16 = provider->configRead16(kIOPCIConfigCommand);

    reg16 |= ( kIOPCICommandBusMaster       |
               kIOPCICommandMemorySpace     |
               kIOPCICommandMemWrInvalidate );

    reg16 &= ~kIOPCICommandIOSpace;  // disable I/O space

    provider->configWrite16( kIOPCIConfigCommand, reg16 );
    provider->findPCICapability(kIOPCIPowerManagementCapability,
                                &pmPCICapPtr);
    if (pmPCICapPtr) {
        UInt16 pciPMCReg = provider->configRead32( pmPCICapPtr ) >> 16;
        if (pciPMCReg & kPCIPMCPMESupportFromD3Cold) {
            magicPacketSupported = true;
        }
        provider->configWrite16((pmPCICapPtr + 4), 0x8000 );
        IOSleep(10);
    }
    return true;
}

bool SimpleMtkWlan::createWorkLoop()
{
    _fWorkloop = IOWorkLoop::workLoop();
    return _fWorkloop != 0;
}

IOWorkLoop *SimpleMtkWlan::getWorkLoop() const
{
    return _fWorkloop;
}

IOReturn SimpleMtkWlan::selectMedium(const IONetworkMedium *medium) {
    setSelectedMedium(medium);
    return kIOReturnSuccess;
}

void SimpleMtkWlan::stop(IOService *provider)
{
    XYTrace("%s\n", __FUNCTION__);
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    super::stop(provider);
    setLinkStatus(kIONetworkLinkValid);
    fHalService->detach(pciNub);
    ether_ifdetach(ifp);
    detachInterface(fNetIf, true);
    OSSafeReleaseNULL(fNetIf);
    releaseAll();
}

void SimpleMtkWlan::releaseAll()
{
    if (fHalService) {
        fHalService->release();
        fHalService = NULL;
    }
    if (_fWorkloop) {
        if (_fCommandGate) {
//            _fCommandGate->disable();
            _fWorkloop->removeEventSource(_fCommandGate);
            _fCommandGate->release();
            _fCommandGate = NULL;
        }
        if (fWatchdogWorkLoop && watchdogTimer) {
            watchdogTimer->cancelTimeout();
            fWatchdogWorkLoop->removeEventSource(watchdogTimer);
            watchdogTimer->release();
            watchdogTimer = NULL;
            fWatchdogWorkLoop->release();
            fWatchdogWorkLoop = NULL;
        }
        _fWorkloop->release();
        _fWorkloop = NULL;
    }
    unregistPM();
}

void SimpleMtkWlan::free()
{
    XYTrace("%s\n", __FUNCTION__);
    if (fHalService != NULL) {
        fHalService->release();
        fHalService = NULL;
    }
    super::free();
}

IOReturn SimpleMtkWlan::enable(IONetworkInterface *netif)
{
    XYTrace("%s\n", __FUNCTION__);
    super::enable(netif);
    _fCommandGate->enable();
    fHalService->enable(netif);
    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
    watchdogTimer->enable();
    return kIOReturnSuccess;
}

static IOReturn shutdownRadioAction(OSObject *owner, void *, void *, void *, void *)
{
    SimpleMtkWlan *dev = OSDynamicCast(SimpleMtkWlan, owner);
    if (dev != NULL)
        dev->shutdownRadio();
    return kIOReturnSuccess;
}

void SimpleMtkWlan::shutdownRadio()
{
    watchdogTimer->cancelTimeout();
    if (fNetIf != NULL)
        fHalService->disable(fNetIf);
    fHalService->shutdownHardware();
}

// Without this the firmware keeps scanning (and the driver logging) through
// power-off until the machine resets, and the card starts the next boot busy.
void SimpleMtkWlan::systemWillShutdown(IOOptionBits specifier)
{
    if ((specifier == kIOMessageSystemWillPowerOff ||
         specifier == kIOMessageSystemWillRestart) && _fCommandGate != NULL) {
        mtk_log("SimpleMtkWlan: system shutdown, turning the radio off\n");
        _fCommandGate->runAction(shutdownRadioAction);
    }
    super::systemWillShutdown(specifier);
}

IOReturn SimpleMtkWlan::disable(IONetworkInterface *netif)
{
    XYTrace("%s\n", __FUNCTION__);
    super::disable(netif);
    watchdogTimer->cancelTimeout();
    watchdogTimer->disable();
    fHalService->disable(netif);
    setLinkStatus(kIONetworkLinkValid);
    return kIOReturnSuccess;
}

bool SimpleMtkWlan::
setLinkStatus(UInt32 status, const IONetworkMedium * activeMedium, UInt64 speed, OSData * data)
{
    _ifnet *ifq = &fHalService->get80211Controller()->ic_ac.ac_if;
    bool ret = super::setLinkStatus(status, activeMedium, speed, data);
    if (fNetIf) {
        if (status & kIONetworkLinkActive) {
#ifdef __PRIVATE_SPI__
            fNetIf->startOutputThread();
#endif
        } else if (!(status & kIONetworkLinkNoNetworkChange)) {
#ifdef __PRIVATE_SPI__
            fNetIf->stopOutputThread();
            fNetIf->flushOutputQueue();
#endif
            ifq_flush(&ifq->if_snd);
            mq_purge(&fHalService->get80211Controller()->ic_mgtq);
        }
    }
    return ret;
}

IOReturn SimpleMtkWlan::getHardwareAddress(IOEthernetAddress *addrP)
{
    if (IEEE80211_ADDR_EQ(etheranyaddr, fHalService->get80211Controller()->ic_myaddr)) {
        return kIOReturnError;
    } else {
        IEEE80211_ADDR_COPY(addrP, fHalService->get80211Controller()->ic_myaddr);
        return kIOReturnSuccess;
    }
}

IOReturn SimpleMtkWlan::setHardwareAddress(const IOEthernetAddress *addrP)
{
    if (!fNetIf || !addrP)
        return kIOReturnError;
    if_setlladdr(&fHalService->get80211Controller()->ic_ac.ac_if, addrP->bytes);
    if (fHalService->get80211Controller()->ic_state > IEEE80211_S_INIT) {
        fHalService->disable(fNetIf);
        fHalService->enable(fNetIf);
    }
    return kIOReturnSuccess;
}

#ifdef __PRIVATE_SPI__
IOReturn SimpleMtkWlan::outputStart(IONetworkInterface *interface, IOOptionBits options)
{
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    mbuf_t m = NULL;
    if (ifq_is_oactive(&ifp->if_snd))
        return kIOReturnNoResources;
    while (kIOReturnSuccess == interface->dequeueOutputPackets(1, &m)) {
        if (outputPacket(m, NULL)!= kIOReturnOutputSuccess ||
            ifq_is_oactive(&ifp->if_snd))
            return kIOReturnNoResources;
    }
    return kIOReturnSuccess;
}
#endif

UInt32 SimpleMtkWlan::outputPacket(mbuf_t m, void *param)
{
//    XYTrace("%s\n", __FUNCTION__);
    IOReturn ret = kIOReturnOutputSuccess;
    _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;

    if (fHalService->get80211Controller()->ic_state != IEEE80211_S_RUN || ifp->if_snd.queue == NULL) {
        if (m && mbuf_type(m) != MBUF_TYPE_FREE) {
            freePacket(m);
        }
        return kIOReturnOutputDropped;
    }
    if (m == NULL) {
        XYLog("%s m==NULL!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        ret = kIOReturnOutputDropped;
    }
    if (!(mbuf_flags(m) & MBUF_PKTHDR) ){
        XYLog("%s pkthdr is NULL!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    if (mbuf_type(m) == MBUF_TYPE_FREE) {
        XYLog("%s mbuf is FREE!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        ret = kIOReturnOutputDropped;
    }
    if (!ifp->if_snd.queue->lockEnqueue(m)) {
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    (*ifp->if_start)(ifp);
    return ret;
}

UInt32 SimpleMtkWlan::getFeatures() const
{
    return fHalService->getDriverInfo()->supportedFeatures();
}

IOReturn SimpleMtkWlan::setPromiscuousMode(IOEnetPromiscuousMode mode)
{
    return kIOReturnSuccess;
}

IOReturn SimpleMtkWlan::setMulticastMode(IOEnetMulticastMode mode)
{
    return kIOReturnSuccess;
}

IOReturn SimpleMtkWlan::setMulticastList(IOEthernetAddress* addr, UInt32 len)
{
    return fHalService->getDriverController()->setMulticastList(addr, len);
}

IOReturn SimpleMtkWlan::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    IOReturn    rtn = kIOReturnSuccess;
    if (group == gIOEthernetWakeOnLANFilterGroup && magicPacketSupported) {
        *filters = kIOEthernetWakeOnMagicPacket;
    } else if (group == gIONetworkFilterGroup) {
        *filters = kIOPacketFilterMulticast | kIOPacketFilterPromiscuous;
    } else {
        rtn = IOEthernetController::getPacketFilters(group, filters);
    }
    return rtn;
}

IOReturn SimpleMtkWlan::
tsleepHandler(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3)
{
    SimpleMtkWlan* dev = OSDynamicCast(SimpleMtkWlan, owner);
    if (dev == 0)
        return kIOReturnError;
    
    if (arg1 == 0) {
        if (_fCommandGate->commandSleep(arg0, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    } else {
        AbsoluteTime deadline;
        clock_interval_to_deadline((*(int*)arg1), kNanosecondScale, reinterpret_cast<uint64_t*> (&deadline));
        if (_fCommandGate->commandSleep(arg0, deadline, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    }
}
