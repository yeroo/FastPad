#pragma once

// Resource IDs (rc.exe + C++). Kept apart from the IDM_* command range in
// commands.h - resources live in their own namespace, but distinct numbers
// avoid confusion when reading logs.
#define IDR_FONT_MAIN 1001
// Application icon: ID 1 so Explorer's "first icon by ID" pick is stable.
#define IDI_APP 1
