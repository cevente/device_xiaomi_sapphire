#pragma once

#include <aidl/vendor/lineage/livedisplay/BnSunlightEnhancement.h>
#include <thread>
#include <atomic>
#include <cstdint>

namespace aidl::vendor::lineage::livedisplay {

class SunlightEnhancement : public BnSunlightEnhancement {
public:
    SunlightEnhancement();
    ~SunlightEnhancement() override;

    // Standard AIDL methods (verify signatures match your interface definition)
    ::ndk::ScopedAStatus isEnabled(bool* _aidl_return) override;
    ::ndk::ScopedAStatus setEnabled(bool enabled) override;

private:
    void monitorScreenState();
    uint32_t getBrightness();
    void setBrightness(uint32_t level);
    void applyHbm(bool enabled);

    std::thread mMonitorThread;
    std::atomic<bool> mStopThread{false};
    uint32_t mStoredBrightness = 0;
};

} // namespace aidl::vendor::lineage::livedisplay
