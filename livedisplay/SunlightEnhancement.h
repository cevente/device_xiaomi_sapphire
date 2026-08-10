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

    // Corrected to match ISunlightEnhancement.aidl (getEnabled instead of isEnabled)
    ::ndk::ScopedAStatus getEnabled(bool* _aidl_return) override;
    ::ndk::ScopedAStatus setEnabled(bool enabled) override;

private:
    void monitorScreenState();
    uint32_t getBrightness();
    void setBrightness(uint32_t level);
    void applyHbm(bool enabled);

    std::thread mMonitorThread;
    std::atomic<bool> mStopThread{false};
    uint32_t mStoredBrightness = 0;
    bool mEnabled = false; // Added missing state tracker variable
};

} // namespace aidl::vendor::lineage::livedisplay
