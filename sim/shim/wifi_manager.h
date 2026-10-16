#pragma once
class WifiManager {
public:
    static WifiManager& GetInstance() { static WifiManager m; return m; }
    void SuspendStationForExternalAp() {}
    void ResumeStationAfterExternalAp() {}
};
