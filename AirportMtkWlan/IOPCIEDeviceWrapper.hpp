//
//  IOPCIEDeviceWrapper.hpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef IOPCIEDeviceWrapper_hpp
#define IOPCIEDeviceWrapper_hpp

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOTypes.h>
#include <IOKit/IOTimerEventSource.h>

#include <HAL/MtkHalService.hpp>

class IOPCIEDeviceWrapper : public IOService {
    OSDeclareDefaultStructors(IOPCIEDeviceWrapper)
    
public:
    virtual IOService* probe(IOService* provider, SInt32* score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual IOWorkLoop* getWorkLoop() const override;
    virtual IOReturn setPowerState(
        unsigned long powerStateOrdinal,
                                   IOService *   whatDevice ) override;
    
public:
    MtkHalService *fHalService;
    IOPCIDevice *pciNub;

private:
    /*
     * The driver log, published on this service every 2 s (AirportMtkWlanLog):
     * IOLog from an OpenCore-injected kext never reaches the unified log, so
     * this is how a test boot without network keeps its log (see
     * mtkwlan-logsaver), also when the Wi-Fi controller fails to start.
     */
    IOTimerEventSource *logTimer;
    void publishLog();
    static void logTimerFired(OSObject *owner, IOTimerEventSource *sender);
};

#endif /* IOPCIEDeviceWrapper_hpp */
