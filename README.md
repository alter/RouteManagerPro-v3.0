# Route Manager Pro

A Windows application for automatic VPN routing of selected applications using the Windows routing table and WinDivert driver.

## Overview

Route Manager Pro automatically routes network traffic from selected applications through a specified VPN gateway while leaving other applications to use the default internet connection. This is achieved by monitoring network connections in real-time and dynamically adding Windows routing table entries.

## Features

- **Selective Application Routing** - Choose which applications should use VPN
- **Real-time Connection Monitoring** - Automatically detects new connections from selected apps
- **Dynamic Route Management** - Creates and manages Windows routing table entries on-the-fly
- **Multiple VPN Support** - Easy gateway switching with automatic route migration
- **System Tray Integration** - Minimizes to system tray for background operation
- **Route Persistence** - Saves and restores routes across application restarts
- **AI Service Preloading** - Optional preloading of IP ranges for services like Discord

## How It Works

### Architecture

The application consists of two main components:

1. **Service Component** (runs in background)
   - `NetworkMonitor` - Uses WinDivert to capture network flow events
   - `RouteController` - Manages Windows routing table entries
   - `ProcessManager` - Tracks selected processes and their states
   - `ConfigManager` - Handles configuration persistence

2. **UI Component** (user interface)
   - Main window for configuration and monitoring
   - System tray icon for quick access
   - Process selection panel
   - Active routes display table

### Technical Flow

```
1. User selects applications (e.g., Discord.exe)
2. NetworkMonitor captures FLOW events via WinDivert
3. When selected app makes connection to IP X:
   - NetworkMonitor detects the connection
   - Notifies RouteController to add route
   - RouteController adds: route add X mask 255.255.255.255 [VPN_GATEWAY]
4. All traffic to IP X now goes through VPN gateway
5. Other applications continue using default route
```

## Technical Implementation

### Core Technologies

- **Language**: C++17
- **Network Capture**: WinDivert 2.2 (kernel-level network packet capture)
- **UI Framework**: Win32 API
- **IPC**: Named Pipes for service-UI communication
- **JSON**: JsonCpp for configuration files
- **Routing**: Windows IP Helper API (iphlpapi)

### Key Components

#### NetworkMonitor (WinDivert Integration)
```cpp
// Captures network flows at FLOW layer
divertHandle = WinDivertOpen("true", WINDIVERT_LAYER_FLOW, 0, 
                            WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
```

#### RouteController (Route Management)
```cpp
// Adds routes using Windows API
CreateIpForwardEntry2(&route);  // Modern API
CreateIpForwardEntry(&route);   // Fallback for older Windows
```

#### Process Caching System
- Two-tier LRU cache for process lookups
- Main cache for active processes
- Miss cache for recently checked processes
- Reduces OpenProcess calls by ~95%

### Thread Architecture

1. **Main Thread** - UI and user interaction
2. **Service Logic Thread** - Core service functionality
3. **Network Monitor Thread** - WinDivert event processing
4. **Route Verification Thread** - Periodic route integrity checks
5. **Process Update Thread** - Process list refresh
6. **Config Persistence Thread** - Configuration auto-save

### Performance Optimizations

- **Efficient Process Lookup**: O(1) average case with caching
- **Batch Route Operations**: Multiple routes processed together
- **Lazy State Persistence**: Deferred saving with dirty flags
- **Interface Caching**: Network interface lookups cached
- **Lock-free Atomics**: For frequently accessed flags

## Building

### Prerequisites

- Windows 10/11 SDK
- Visual Studio 2019 or later
- CMake 3.20+
- Admin privileges (required for route manipulation)

### Dependencies

- WinDivert 2.2.0
- JsonCpp 1.9.5
- Windows IP Helper API
- WinSock2

### Build Steps

```bash
# Clone repository
git clone https://github.com/yourusername/route-manager-pro.git
cd route-manager-pro

# Create build directory
mkdir build && cd build

# Generate project files
cmake ..

# Build
cmake --build . --config Release
```

## Configuration Files

### config.json
```json
{
  "gatewayIp": "10.200.210.1",
  "metric": 1,
  "selectedProcesses": ["Discord.exe", "Steam.exe"],
  "startMinimized": true,
  "aiPreloadEnabled": false
}
```

### state.json
```
version=3
gateway=10.200.210.1
route=162.159.128.233,Discord.exe,1699564234,32,10.200.210.1
```

### preload_ips.json
```json
{
  "services": [{
    "name": "Discord",
    "enabled": true,
    "ranges": ["162.159.128.0/19"]
  }]
}
```

## Security Considerations

- **Requires Administrator privileges** for route table modifications
- **Kernel driver** (WinDivert) requires driver signature
- **No packet inspection** - only monitors connection metadata
- **Local IPC only** - no network services exposed

## Known Limitations

1. **IPv4 Only** - IPv6 routes are not supported
2. **TCP/UDP Only** - Other protocols not routed
3. **Windows Only** - No cross-platform support
4. **Single Gateway** - All selected apps use same VPN gateway

## Troubleshooting

### Common Issues

1. **"Failed to open WinDivert handle"**
   - Ensure WinDivert driver is installed
   - Run as Administrator
   - Check if Windows Defender is blocking

2. **Routes not persisting**
   - Check write permissions in app directory
   - Verify config.json is valid JSON

3. **High CPU usage**
   - Reduce process update frequency
   - Check for process scanning loops

## License

This project is licensed under the MIT License - see LICENSE file for details.

## Acknowledgments

- WinDivert by Vasily Polikhronov
- JsonCpp by Baptiste Lepilleur
- Windows IP Helper documentation by Microsoft