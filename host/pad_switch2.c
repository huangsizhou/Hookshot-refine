/* (hookshot) The Nintendo Switch 2 Pro Controller over USB (057E:2069) on Windows, read by the host itself.

   SDL has a driver for it (src/joystick/hidapi/SDL_hidapi_switch2.c, Valve's, zlib license) but builds it only with libusb,
   which SDL's Windows releases are made without: the SDL 3.4.20 beside the game does not see the controller, and the game
   saw nothing from it over USB. The controller sends no input until it is told to start, over the two bulk endpoints of
   its USB interface 1, and Windows binds that interface to WinUSB by itself (the controller's own Microsoft OS descriptors
   name its device interface, GUID_SWITCH2 below); interface 0 is an ordinary HID device that then sends a 64-byte report
   every 4 ms. So nothing is installed: WinUSB starts it and reads its calibration, the HID interface gives the reports and
   takes the rumble, both through Windows' own DLLs, loaded at run time. The commands, the report's layout, the stick and
   gyroscope calibration in the controller's flash and the rumble packet are SDL's driver's.

   Its pads are listed with SDL's (host/hos_sdl.c hos_sdl_pads_list) and the hos_sdl_pad_* calls for one come here, so
   host/pad_sdl.c treats it as any other pad: buttons by position (B at the bottom), sticks, ZL/ZR as full triggers, the
   gyroscope and accelerometer for Motion controls, rumble on its two motors (as SDL's driver plays it), and its name for
   the Switch button prompts. All on pad_sdl.c's polling thread, the only one that calls these.

   Not done: the C button (no input for it), Joy-Con 2 and the GameCube controller (other reports), Bluetooth (SDL's driver
   has none either). Another program holding the WinUSB interface (Steam Input, which has a driver of its own for the
   controller) keeps it from starting here: the log says so, and Steam's pad is read through SDL as before.
   HOS_SWITCH2=0: the controller is not looked for. */
#include "hos.h"
#include "pad_switch2.h"
#include "hos_sdl.h"
#include <stdio.h>
#include <string.h>
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif

#ifdef _WIN32
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <winusb.h>
#include <hidsdi.h>
#include <hidpi.h>

/* the device interface Windows gives the controller's WinUSB interface (its MS OS 2.0 descriptor's DeviceInterfaceGUID) */
static const GUID GUID_SWITCH2 = {0x6F13725E, 0xEF0E, 0x4FD3, {0xAE, 0x5F, 0xB2, 0xDE, 0x98, 0x9E, 0xC8, 0x25}};
static const GUID GUID_HID = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
static const wchar_t PRO_IDS[] = L"vid_057e&pid_2069";

enum { SW2_MAX = 4, SW2_CODE = 0x7E052069 };   /* (an open code and id far from SDL's: SDL3 counts its ids up from 1) */
enum { B_SOUTH, B_EAST, B_WEST, B_NORTH, B_BACK, B_GUIDE, B_START, B_LSTICK, B_RSTICK, B_LSHOULDER, B_RSHOULDER, B_UP,
       B_DOWN, B_LEFT, B_RIGHT, B_MISC1, B_RPADDLE1, B_LPADDLE1 };
enum { A_LX, A_LY, A_RX, A_RY, A_LT, A_RT };

typedef struct { uint16_t neutral, max, min; } AxisCal;

typedef struct {
    int found;                /* seen at the last scan */
    int open, gone;           /* handed to pad_sdl.c; its reads failed (unplugged) */
    DEVINST composite;        /* the USB device both interfaces belong to */
    wchar_t wu_path[512], hid_path[512];
    int64_t retry_ns;         /* an open that failed: not before */
    HANDLE wu_file, hid;
    WINUSB_INTERFACE_HANDLE wu;
    UCHAR ep_out, ep_in;
    OVERLAPPED rov, wov;
    int reading, writing;
    uint8_t rbuf[128], wbuf[128];
    DWORD in_len, out_len;
    int off;                  /* 1: Windows put a report id 0 before the controller's bytes (a descriptor without ids) */
    int out_numbered;
    uint32_t buttons;
    int16_t axes[6];
    AxisCal cal[4];           /* left x, left y, right x, right y */
    float gyro_bias[3];
    int samples, ready;       /* the sensors' clock measured (SDL's way): the gyroscope's scale follows from it */
    uint32_t first_ts, last_ts;
    float gyro_coeff, accel[3], gyro[3];
    int have_motion;
    uint16_t rumble_lo, rumble_hi;
    int64_t rumble_next;
    uint32_t rumble_seq;
    int rumble_dirty;
    uint64_t reports;
    char name[64];
} Sw2;
static Sw2 sw2[SW2_MAX];

/* ---- Windows' DLLs, loaded once ---- */
static struct {
    HDEVINFO (WINAPI *GetClassDevs)(const GUID *, PCWSTR, HWND, DWORD);
    BOOL (WINAPI *EnumInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA);
    BOOL (WINAPI *InterfaceDetail)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD,
                                   PSP_DEVINFO_DATA);
    BOOL (WINAPI *DestroyList)(HDEVINFO);
    CONFIGRET (WINAPI *Parent)(PDEVINST, DEVINST, ULONG);
    BOOL (WINAPI *WuInit)(HANDLE, PWINUSB_INTERFACE_HANDLE);
    BOOL (WINAPI *WuFree)(WINUSB_INTERFACE_HANDLE);
    BOOL (WINAPI *WuSettings)(WINUSB_INTERFACE_HANDLE, UCHAR, PUSB_INTERFACE_DESCRIPTOR);
    BOOL (WINAPI *WuPipe)(WINUSB_INTERFACE_HANDLE, UCHAR, UCHAR, PWINUSB_PIPE_INFORMATION);
    BOOL (WINAPI *WuPolicy)(WINUSB_INTERFACE_HANDLE, UCHAR, ULONG, ULONG, PVOID);
    BOOL (WINAPI *WuWrite)(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR, ULONG, PULONG, LPOVERLAPPED);
    BOOL (WINAPI *WuRead)(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR, ULONG, PULONG, LPOVERLAPPED);
    BOOLEAN (WINAPI *HidPreparsed)(HANDLE, PHIDP_PREPARSED_DATA *);
    BOOLEAN (WINAPI *HidFreePreparsed)(PHIDP_PREPARSED_DATA);
    BOOLEAN (WINAPI *HidBuffers)(HANDLE, ULONG);
    NTSTATUS (WINAPI *HidCaps)(PHIDP_PREPARSED_DATA, PHIDP_CAPS);
    NTSTATUS (WINAPI *HidValueCaps)(HIDP_REPORT_TYPE, PHIDP_VALUE_CAPS, PUSHORT, PHIDP_PREPARSED_DATA);
    NTSTATUS (WINAPI *HidButtonCaps)(HIDP_REPORT_TYPE, PHIDP_BUTTON_CAPS, PUSHORT, PHIDP_PREPARSED_DATA);
} w;

static int load(void) {
    static int state;   /* 0 not tried, 1 loaded, -1 not available */
    if (state) return state > 0;
    state = -1;
    HMODULE su = LoadLibraryW(L"setupapi.dll"), cm = LoadLibraryW(L"cfgmgr32.dll"), wu = LoadLibraryW(L"winusb.dll"),
            hd = LoadLibraryW(L"hid.dll");
    if (!su || !cm || !wu || !hd) {
        hos_log("hid: Switch 2 Pro Controller (USB): Windows' setupapi, cfgmgr32, winusb or hid DLL is missing: not looked for");
        return 0;
    }
#define GET(m, field, name) if (!(*(FARPROC *)&w.field = GetProcAddress(m, name))) { \
        hos_log("hid: Switch 2 Pro Controller (USB): no %s: not looked for", name); return 0; }
    GET(su, GetClassDevs, "SetupDiGetClassDevsW");
    GET(su, EnumInterfaces, "SetupDiEnumDeviceInterfaces");
    GET(su, InterfaceDetail, "SetupDiGetDeviceInterfaceDetailW");
    GET(su, DestroyList, "SetupDiDestroyDeviceInfoList");
    GET(cm, Parent, "CM_Get_Parent");
    GET(wu, WuInit, "WinUsb_Initialize");
    GET(wu, WuFree, "WinUsb_Free");
    GET(wu, WuSettings, "WinUsb_QueryInterfaceSettings");
    GET(wu, WuPipe, "WinUsb_QueryPipe");
    GET(wu, WuPolicy, "WinUsb_SetPipePolicy");
    GET(wu, WuWrite, "WinUsb_WritePipe");
    GET(wu, WuRead, "WinUsb_ReadPipe");
    GET(hd, HidPreparsed, "HidD_GetPreparsedData");
    GET(hd, HidFreePreparsed, "HidD_FreePreparsedData");
    GET(hd, HidBuffers, "HidD_SetNumInputBuffers");
    GET(hd, HidCaps, "HidP_GetCaps");
    GET(hd, HidValueCaps, "HidP_GetValueCaps");
    GET(hd, HidButtonCaps, "HidP_GetButtonCaps");
#undef GET
    state = 1;
    return 1;
}

static int enabled(void) {
    static int v = -1;
    if (v < 0) v = !(getenv("HOS_SWITCH2") && !env_on("HOS_SWITCH2")) && load();
    return v;
}

/* ---- finding them: the WinUSB interface and the HID interface of one USB device, paired by that device ---- */
static int is_pro(const wchar_t *path) {
    wchar_t low[512];
    size_t i = 0;
    for (; path[i] && i < 511; i++) low[i] = (wchar_t)(path[i] >= L'A' && path[i] <= L'Z' ? path[i] + 32 : path[i]);
    low[i] = 0;
    return wcsstr(low, PRO_IDS) != NULL;
}

typedef struct { DEVINST composite; wchar_t path[512]; } Found;

/* the interfaces of one kind (guid) on a Pro Controller, with the USB device each is part of (levels: parents up to it) */
static int find(const GUID *guid, int levels, Found *out, int max) {
    HDEVINFO set = w.GetClassDevs(guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    SP_DEVICE_INTERFACE_DATA ifd = {sizeof ifd};
    for (DWORD i = 0; n < max && w.EnumInterfaces(set, NULL, guid, i, &ifd); i++) {
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; char bytes[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 1024]; } u;
        u.d.cbSize = sizeof u.d;
        SP_DEVINFO_DATA info = {sizeof info};
        if (!w.InterfaceDetail(set, &ifd, &u.d, sizeof u, NULL, &info) || !is_pro(u.d.DevicePath)) continue;
        DEVINST at = info.DevInst;
        int ok = 1;
        for (int l = 0; l < levels && ok; l++) ok = w.Parent(&at, at, 0) == CR_SUCCESS;
        if (!ok) continue;
        out[n].composite = at;
        wcsncpy(out[n].path, u.d.DevicePath, 511);
        out[n].path[511] = 0;
        n++;
    }
    w.DestroyList(set);
    return n;
}

/* (every 2 s from the list: the WinUSB interfaces first, a short list; all of the HID devices only when there is one) */
static void scan(void) {
    Found wus[SW2_MAX], hids[SW2_MAX];
    const int nw = find(&GUID_SWITCH2, 1, wus, SW2_MAX);        /* (the interface's node, then the USB device) */
    const int nh = nw ? find(&GUID_HID, 2, hids, SW2_MAX) : 0;   /* (the HID node, the USB interface, then the USB device) */
    for (int k = 0; k < SW2_MAX; k++) sw2[k].found = 0;
    for (int i = 0; i < nw; i++)
        for (int j = 0; j < nh; j++) {
            if (wus[i].composite != hids[j].composite) continue;
            int k = -1;   /* (its slot: the one it had, open or waiting to try again, else a free one) */
            for (int q = 0; q < SW2_MAX && k < 0; q++)
                if ((sw2[q].open || sw2[q].retry_ns) && sw2[q].composite == wus[i].composite) k = q;
            for (int q = 0; q < SW2_MAX && k < 0; q++)
                if (!sw2[q].open && !sw2[q].retry_ns && !sw2[q].found) k = q;
            if (k >= 0) {
                sw2[k].found = 1;
                if (!sw2[k].open) {
                    sw2[k].composite = wus[i].composite;
                    memcpy(sw2[k].wu_path, wus[i].path, sizeof wus[i].path);
                    memcpy(sw2[k].hid_path, hids[j].path, sizeof hids[j].path);
                }
            }
            break;
        }
    for (int k = 0; k < SW2_MAX; k++)
        if (!sw2[k].found && !sw2[k].open) sw2[k].retry_ns = 0;   /* (unplugged: a new one gets a fresh try) */
}

/* ---- the bulk endpoints: SDL's SendBulkData / RecvBulkData ---- */
static int bulk_send(Sw2 *d, const uint8_t *data, unsigned size) {
    ULONG done = 0;
    return w.WuWrite(d->wu, d->ep_out, (PUCHAR)data, size, &done, NULL) ? (int)done : -1;
}
/* in reads of 64 bytes, as the controller sends them, until a short one */
static int bulk_recv(Sw2 *d, uint8_t *data, unsigned size) {
    int total = 0;
    while (size > 0) {
        const unsigned want = size > 64 ? 64 : size;
        ULONG got = 0;
        if (!w.WuRead(d->wu, d->ep_in, data, want, &got, NULL)) return total ? total : -1;
        total += (int)got;
        size -= got;
        data += want;
        if (got < want) break;
    }
    return total;
}

/* 0x40 bytes of the controller's flash from address (SDL's ReadFlashBlock); 0 when read */
static int read_flash(Sw2 *d, uint32_t address, uint8_t out[0x40]) {
    uint8_t cmd[16] = {0x02, 0x91, 0x00, 0x01, 0x00, 0x08, 0x00, 0x00};
    uint8_t reply[0x50] = {0};
    cmd[12] = (uint8_t)address, cmd[13] = (uint8_t)(address >> 8), cmd[14] = (uint8_t)(address >> 16),
    cmd[15] = (uint8_t)(address >> 24);
    if (bulk_send(d, cmd, sizeof cmd) < 0 || bulk_recv(d, reply, sizeof reply) < 0x50) return -1;
    memcpy(out, reply + 0x10, 0x40);
    return 0;
}

/* a stick's calibration: its x and y neutral, the reach above it and below it, 12 bits each (SDL's ParseStickCalibration) */
static void parse_stick(AxisCal *x, AxisCal *y, const uint8_t *p) {
    x->neutral = (uint16_t)(p[0] | (p[1] & 0x0F) << 8), y->neutral = (uint16_t)(p[1] >> 4 | p[2] << 4);
    x->max = (uint16_t)(p[3] | (p[4] & 0x0F) << 8), y->max = (uint16_t)(p[4] >> 4 | p[5] << 4);
    x->min = (uint16_t)(p[6] | (p[7] & 0x0F) << 8), y->min = (uint16_t)(p[7] >> 4 | p[8] << 4);
}

static void calibrate(Sw2 *d) {
    uint8_t b[0x40];
    float f[3];
    if (!read_flash(d, 0x13040, b)) memcpy(f, b + 4, sizeof f), memcpy(d->gyro_bias, f, sizeof f);
    if (!read_flash(d, 0x13080, b)) parse_stick(&d->cal[0], &d->cal[1], b + 0x28);
    if (!read_flash(d, 0x130C0, b)) parse_stick(&d->cal[2], &d->cal[3], b + 0x28);
    /* (the player's own calibration from the console's settings, when there is one) */
    if (!read_flash(d, 0x1FC040, b) && b[0] == 0xb2 && b[1] == 0xa1) parse_stick(&d->cal[0], &d->cal[1], b + 2);
    if (!read_flash(d, 0x1FC080, b) && b[0] == 0xb2 && b[1] == 0xa1) parse_stick(&d->cal[2], &d->cal[3], b + 2);
}

/* SDL's start sequence, in its order; each command's length is 8 + its byte 5 */
static const uint8_t START[][28] = {
    {0x07, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00},
    {0x0c, 0x91, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x27, 0x00, 0x00, 0x00},   /* (the features' mask: buttons, sticks, motion) */
    {0x11, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00},
    {0x0a, 0x91, 0x00, 0x08, 0x00, 0x14, 0x00, 0x00, 0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
     0xff, 0x35, 0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x0c, 0x91, 0x00, 0x04, 0x00, 0x04, 0x00, 0x00, 0x27, 0x00, 0x00, 0x00},   /* (those features on) */
    {0x01, 0x91, 0x00, 0x0c, 0x00, 0x00, 0x00, 0x00},
    {0x01, 0x91, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00},                           /* (rumble on) */
    {0x08, 0x91, 0x00, 0x02, 0x00, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00},
    {0x03, 0x91, 0x00, 0x0a, 0x00, 0x04, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00},   /* (the report's format) */
    {0x03, 0x91, 0x00, 0x0d, 0x00, 0x08, 0x00, 0x00, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},   /* (reports on) */
};

/* ---- one pad opened, closed ---- */
static void shut(Sw2 *d) {
    if (d->hid && d->hid != INVALID_HANDLE_VALUE) {
        CancelIo(d->hid);
        CloseHandle(d->hid);
    }
    if (d->wu) w.WuFree(d->wu);
    if (d->wu_file && d->wu_file != INVALID_HANDLE_VALUE) CloseHandle(d->wu_file);
    if (d->rov.hEvent) CloseHandle(d->rov.hEvent);
    if (d->wov.hEvent) CloseHandle(d->wov.hEvent);
    d->hid = d->wu_file = NULL, d->wu = NULL;
    d->rov.hEvent = d->wov.hEvent = NULL;
    d->reading = d->writing = 0;
}

static const char *open_pad(Sw2 *d) {
    d->ep_in = d->ep_out = 0;
    memset(d->cal, 0, sizeof d->cal);
    memset(d->gyro_bias, 0, sizeof d->gyro_bias);
    d->wu_file =CreateFileW(d->wu_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    if (d->wu_file == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_ACCESS_DENIED ? "another program has it (Steam Input?)" : "its WinUSB interface would not open";
    if (!w.WuInit(d->wu_file, &d->wu)) return "WinUSB would not start on it";
    USB_INTERFACE_DESCRIPTOR ifd;
    if (!w.WuSettings(d->wu, 0, &ifd)) return "its USB interface could not be read";
    for (UCHAR i = 0; i < ifd.bNumEndpoints; i++) {
        WINUSB_PIPE_INFORMATION pipe;
        if (!w.WuPipe(d->wu, 0, i, &pipe) || pipe.PipeType != UsbdPipeTypeBulk) continue;
        if (pipe.PipeId & 0x80) d->ep_in = pipe.PipeId;
        else d->ep_out = pipe.PipeId;
    }
    if (!d->ep_in || !d->ep_out) return "its bulk endpoints were not found";
    ULONG ms = 1000;
    w.WuPolicy(d->wu, d->ep_out, PIPE_TRANSFER_TIMEOUT, sizeof ms, &ms);
    ms = 100;
    w.WuPolicy(d->wu, d->ep_in, PIPE_TRANSFER_TIMEOUT, sizeof ms, &ms);

    d->hid = CreateFileW(d->hid_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED, NULL);
    if (d->hid == INVALID_HANDLE_VALUE) return "its HID interface would not open";
    PHIDP_PREPARSED_DATA pp = NULL;
    if (!w.HidPreparsed(d->hid, &pp)) return "its HID descriptor could not be read";
    HIDP_CAPS caps;
    const NTSTATUS st = w.HidCaps(pp, &caps);
    int numbered_in = 0, numbered_out = 0;
    if (st == HIDP_STATUS_SUCCESS) {
        HIDP_VALUE_CAPS vc[32];
        HIDP_BUTTON_CAPS bc[32];
        USHORT n = 32;
        if (w.HidValueCaps(HidP_Input, vc, &n, pp) == HIDP_STATUS_SUCCESS)
            for (USHORT i = 0; i < n; i++) numbered_in |= vc[i].ReportID != 0;
        n = 32;
        if (w.HidButtonCaps(HidP_Input, bc, &n, pp) == HIDP_STATUS_SUCCESS)
            for (USHORT i = 0; i < n; i++) numbered_in |= bc[i].ReportID != 0;
        n = 32;
        if (w.HidValueCaps(HidP_Output, vc, &n, pp) == HIDP_STATUS_SUCCESS)
            for (USHORT i = 0; i < n; i++) {
                numbered_out |= vc[i].ReportID != 0;
                if (env_on("HOS_SWITCH2_LOG"))
                    hos_log("hid: Switch 2 Pro Controller: output value: report %u, usage page %#x, %u x %u bits",
                            vc[i].ReportID, vc[i].UsagePage, vc[i].ReportCount, vc[i].BitSize);
            }
        n = 32;
        if (w.HidButtonCaps(HidP_Output, bc, &n, pp) == HIDP_STATUS_SUCCESS)
            for (USHORT i = 0; i < n; i++) numbered_out |= bc[i].ReportID != 0;
        if (env_on("HOS_SWITCH2_LOG"))
            hos_log("hid: Switch 2 Pro Controller: input %u bytes, output %u bytes, feature %u bytes", caps.InputReportByteLength,
                    caps.OutputReportByteLength, caps.FeatureReportByteLength);
    }
    w.HidFreePreparsed(pp);
    if (st != HIDP_STATUS_SUCCESS) return "its HID descriptor could not be read";
    d->in_len = caps.InputReportByteLength, d->out_len = caps.OutputReportByteLength;
    if (d->in_len < 64 || d->in_len > sizeof d->rbuf || d->out_len > sizeof d->wbuf) return "its reports are not the size expected";
    /* (Windows' reports start with the report's id, or with 0 when the descriptor has none; SDL's offsets count from the
       controller's first byte) */
    d->off = numbered_in ? 0 : 1;
    d->out_numbered = numbered_out;
    w.HidBuffers(d->hid, 64);
    d->rov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    d->wov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

    calibrate(d);
    uint8_t reply[0x40];
    for (size_t i = 0; i < sizeof START / sizeof START[0]; i++) {
        if (bulk_send(d, START[i], 8u + START[i][5]) < 0) return "it did not take the start commands";
        bulk_recv(d, reply, sizeof reply);
    }
    {   /* (the first player's light, as on the console) */
        uint8_t led[16] = {0x09, 0x91, 0x00, 0x07, 0x00, 0x08, 0x00, 0x00, 0x01};
        if (bulk_send(d, led, sizeof led) >= 0) bulk_recv(d, reply, 8);
    }
    return NULL;
}

/* ---- the reports ---- */
static int16_t stick(const AxisCal *c, int v, int invert) {
    float f = (float)v;
    int16_t m;
    if (c->neutral && c->min && c->max) {
        f -= c->neutral;
        f = f < 0 ? f / c->min : f / c->max;
        f *= 32767.0f;
        m = (int16_t)(f < -32768.0f ? -32768 : f > 32767.0f ? 32767 : f);
    } else {
        m = (int16_t)(v * 65535 / 4096 - 32768);
    }
    return invert ? (int16_t)~m : m;
}

static void parse(Sw2 *d, const uint8_t *r) {
    {   /* (HOS_SWITCH2_LOG=1: the button bytes as they change, for a button this does not know) */
        static int log = -1;
        static uint8_t last[4];
        if (log < 0) log = env_on("HOS_SWITCH2_LOG");
        if (log && memcmp(last, r + 5, 4)) {
            memcpy(last, r + 5, 4);
            hos_log("hid: Switch 2 Pro Controller: buttons %02x %02x %02x %02x", r[5], r[6], r[7], r[8]);
        }
    }
    uint32_t b = 0;
#define BIT(byte, mask, button) (b |= r[byte] & (mask) ? 1u << (button) : 0u)
    BIT(5, 0x01, B_WEST), BIT(5, 0x02, B_NORTH), BIT(5, 0x04, B_SOUTH), BIT(5, 0x08, B_EAST), BIT(5, 0x40, B_RSHOULDER);
    BIT(6, 0x01, B_BACK), BIT(6, 0x02, B_START), BIT(6, 0x04, B_RSTICK), BIT(6, 0x08, B_LSTICK), BIT(6, 0x10, B_GUIDE);
    BIT(6, 0x20, B_MISC1);   /* (the capture button, as SDL gives a Switch Pro Controller's) */
    BIT(7, 0x01, B_DOWN), BIT(7, 0x02, B_UP), BIT(7, 0x04, B_RIGHT), BIT(7, 0x08, B_LEFT), BIT(7, 0x40, B_LSHOULDER);
    BIT(8, 0x01, B_RPADDLE1), BIT(8, 0x02, B_LPADDLE1);   /* (GR and GL on the back) */
#undef BIT
    d->buttons = b;
    d->axes[A_LT] = (int16_t)(r[7] & 0x80 ? 32767 : 0);
    d->axes[A_RT] = (int16_t)(r[5] & 0x80 ? 32767 : 0);
    d->axes[A_LX] = stick(&d->cal[0], r[11] | (r[12] & 0x0F) << 8, 0);
    d->axes[A_LY] = stick(&d->cal[1], r[12] >> 4 | r[13] << 4, 1);
    d->axes[A_RX] = stick(&d->cal[2], r[14] | (r[15] & 0x0F) << 8, 0);
    d->axes[A_RY] = stick(&d->cal[3], r[15] >> 4 | r[16] << 4, 1);

    /* motion (SDL's HandleStatePacket): the sensors' clock is microseconds on most, something else on some; the
       gyroscope's full scale follows which, measured over the first hundred readings */
    const uint32_t ts = (uint32_t)(r[0x2b] | r[0x2c] << 8 | r[0x2d] << 16 | (uint32_t)r[0x2e] << 24);
    if (!ts) return;
    d->last_ts = ts;
    if (!d->ready) {
        d->samples++;
        if (d->samples >= 5 && !d->first_ts) {
            d->first_ts = ts, d->samples = 0;
        } else if (d->samples == 100) {
            const uint64_t coeff = 1000ull * (uint32_t)(ts - d->first_ts) / (d->samples * 4ull);
            if ((coeff + 100000) / 200000 == 5) d->gyro_coeff = 34.8f, d->ready = 1;
            else if (coeff) d->gyro_coeff = 40.0f, d->ready = 1;
            else d->first_ts = 0, d->samples = 0;
            if (d->ready) hos_log("hid: Switch 2 Pro Controller: motion sensors ready (gyroscope scale %.1f)", d->gyro_coeff);
        }
        if (!d->ready) return;
    }
    const float accel = 9.80665f * 8.0f / 32767.0f, gyro = d->gyro_coeff / 32767.0f;
#define S16(o) ((float)(int16_t)(r[o] | r[(o) + 1] << 8))
    d->accel[0] = S16(0x31) * accel;
    d->accel[1] = S16(0x35) * accel;
    d->accel[2] = S16(0x33) * -accel;
    d->gyro[0] = S16(0x37) * gyro - d->gyro_bias[0];
    d->gyro[1] = S16(0x3b) * gyro - d->gyro_bias[2];
    d->gyro[2] = S16(0x39) * -gyro + d->gyro_bias[1];
#undef S16
    d->have_motion = 1;
}

/* SDL's EncodeHDRumble: both bands at fixed frequencies, the amplitudes the motors' */
static void rumble_send(Sw2 *d, int64_t now) {
    if (d->writing) {
        DWORD n = 0;
        if (!GetOverlappedResult(d->hid, &d->wov, &n, FALSE)) {
            if (GetLastError() == ERROR_IO_INCOMPLETE) return;
            static int told;
            if (told++ < 4) hos_log("hid: Switch 2 Pro Controller: its rumble was not taken (error %lu)", GetLastError());
        }
        d->writing = 0;
    }
    if (!d->rumble_dirty && !d->rumble_lo && !d->rumble_hi) return;
    if (now < d->rumble_next) return;
    const uint16_t lo = (uint16_t)(d->rumble_lo * 29000u / 65535u), hi = (uint16_t)(d->rumble_hi * 29000u / 65535u);
    const uint16_t hf = 0x187, lf = 0x112;
    uint8_t p[64] = {0};
    p[0] = 0x02;
    p[1] = (uint8_t)(0x50 | (d->rumble_seq++ & 0xf));
    p[2] = (uint8_t)(hf & 0xFF);
    p[3] = (uint8_t)(((hi >> 4) & 0xfc) | ((hf >> 8) & 0x03));
    p[4] = (uint8_t)((hi >> 12) | (lf << 4));
    p[5] = (uint8_t)((lo & 0xc0) | ((lf >> 4) & 0x3f));
    p[6] = (uint8_t)(lo >> 8);
    memcpy(p + 0x11, p + 0x01, 6);   /* (the second motor's half: the same) */
    memset(d->wbuf, 0, sizeof d->wbuf);
    if (d->out_numbered) memcpy(d->wbuf, p, d->out_len < 64 ? d->out_len : 64);
    else memcpy(d->wbuf + 1, p, d->out_len - 1 < 64 ? d->out_len - 1 : 64);
    ResetEvent(d->wov.hEvent);
    if (WriteFile(d->hid, d->wbuf, d->out_len, NULL, &d->wov) || GetLastError() == ERROR_IO_PENDING) {
        d->writing = 1;
    } else {
        static int told;
        if (told++ < 4) hos_log("hid: Switch 2 Pro Controller: its rumble could not be sent (error %lu)", GetLastError());
    }
    d->rumble_dirty = 0;
    d->rumble_next = d->rumble_lo || d->rumble_hi ? now + 12000000 : 0;
}

static void poll_pad(Sw2 *d) {
    for (int guard = 0; guard < 256; guard++) {
        DWORD n = 0;
        if (!d->reading) {
            ResetEvent(d->rov.hEvent);
            if (!ReadFile(d->hid, d->rbuf, d->in_len, NULL, &d->rov) && GetLastError() != ERROR_IO_PENDING) {
                d->gone = 1;
                return;
            }
            d->reading = 1;
        }
        if (!GetOverlappedResult(d->hid, &d->rov, &n, FALSE)) {
            if (GetLastError() != ERROR_IO_INCOMPLETE) d->gone = 1;
            return;
        }
        d->reading = 0;
        if (n < (DWORD)(64 + d->off)) continue;
        if (!d->reports++) {
            char hex[3 * 24 + 1];
            for (int i = 0; i < 24; i++) snprintf(hex + 3 * i, 4, "%02x ", d->rbuf[i]);
            hos_log("hid: Switch 2 Pro Controller: reports coming (%lu bytes, %s ids; the first: %s...)", (unsigned long)n,
                    d->off ? "without" : "with", hex);
        }
        parse(d, d->rbuf + d->off);
    }
}

/* ---- what host/hos_sdl.c calls ---- */
static Sw2 *pad_of(const void *pad) {
    return pad >= (const void *)sw2 && pad < (const void *)(sw2 + SW2_MAX) ? (Sw2 *)pad : NULL;
}
int hos_switch2_is(const void *pad) { return pad_of(pad) != NULL; }
int hos_switch2_code(int what) { return what >= SW2_CODE && what < SW2_CODE + SW2_MAX; }

int hos_switch2_list(int32_t *ids, int *opens, int max) {
    if (!enabled()) return 0;
    static int64_t next_scan;
    const int64_t now = hos_now_ns();
    if (now >= next_scan) {
        next_scan = now + 2000000000LL;
        scan();
    }
    int n = 0;
    for (int k = 0; k < SW2_MAX && n < max; k++)
        if ((sw2[k].found || sw2[k].open) && (sw2[k].open || now >= sw2[k].retry_ns))
            ids[n] = opens[n] = SW2_CODE + k, n++;
    return n;
}

void *hos_switch2_open(int what) {
    if (!hos_switch2_code(what) || !enabled()) return NULL;
    Sw2 *d = &sw2[what - SW2_CODE];
    if (d->open) return d;
    const int64_t t0 = hos_now_ns();
    const char *why = open_pad(d);
    if (why) {
        shut(d);
        const int first = !d->retry_ns;
        d->retry_ns = hos_now_ns() + 10000000000LL;   /* (another try in ten seconds, said once) */
        if (first) hos_log("hid: Switch 2 Pro Controller (USB) found, but %s: trying again every 10 s", why);
        return NULL;
    }
    d->open = 1, d->gone = 0, d->retry_ns = 0;
    d->buttons = 0;
    memset(d->axes, 0, sizeof d->axes);
    d->samples = d->ready = 0, d->first_ts = d->last_ts = 0, d->gyro_coeff = 34.8f, d->have_motion = 0;
    d->rumble_lo = d->rumble_hi = 0, d->rumble_next = 0, d->rumble_dirty = 0, d->reports = 0;
    snprintf(d->name, sizeof d->name, "Nintendo Switch 2 Pro Controller");
    hos_log("hid: Switch 2 Pro Controller started over USB in %.0f ms (WinUSB endpoints %#x/%#x, reports %lu bytes, "
            "left stick neutral %u,%u, right %u,%u)", (hos_now_ns() - t0) / 1e6, d->ep_out, d->ep_in,
            (unsigned long)d->in_len, d->cal[0].neutral, d->cal[1].neutral, d->cal[2].neutral, d->cal[3].neutral);
    return d;
}

void hos_switch2_close(void *pad) {
    Sw2 *d = pad_of(pad);
    if (!d) return;
    shut(d);
    d->open = 0, d->gone = 0;
}

int hos_switch2_connected(void *pad) {
    Sw2 *d = pad_of(pad);
    return d && d->open && !d->gone;
}

void hos_switch2_update(void) {
    const int64_t now = hos_now_ns();
    for (int k = 0; k < SW2_MAX; k++) {
        Sw2 *d = &sw2[k];
        if (!d->open || d->gone) continue;
        poll_pad(d);
        if (!d->gone) rumble_send(d, now);
    }
}

int hos_switch2_button(void *pad, int button) {
    Sw2 *d = pad_of(pad);
    return d && button >= 0 && button < 32 && (d->buttons >> button & 1);
}
int16_t hos_switch2_axis(void *pad, int axis) {
    Sw2 *d = pad_of(pad);
    return d && axis >= 0 && axis < 6 ? d->axes[axis] : 0;
}
const char *hos_switch2_name(void *pad) {
    Sw2 *d = pad_of(pad);
    return d ? d->name : "?";
}
int32_t hos_switch2_id(void *pad) {
    Sw2 *d = pad_of(pad);
    return d ? SW2_CODE + (int32_t)(d - sw2) : 0;
}
void hos_switch2_rumble(void *pad, uint16_t low, uint16_t high) {
    Sw2 *d = pad_of(pad);
    if (!d || (low == d->rumble_lo && high == d->rumble_hi)) return;
    d->rumble_lo = low, d->rumble_hi = high, d->rumble_dirty = 1, d->rumble_next = 0;
}
int hos_switch2_sensor(void *pad, int type, float *data, int n) {
    Sw2 *d = pad_of(pad);
    if (!d || !d->have_motion || n < 3) return 0;
    memcpy(data, type == HOS_SDL_SENSOR_GYRO ? d->gyro : d->accel, 3 * sizeof(float));
    return 1;
}

#else   /* (other systems: SDL with libusb has its own driver for it) */
int hos_switch2_list(int32_t *ids, int *opens, int max) { (void)ids, (void)opens, (void)max; return 0; }
int hos_switch2_code(int what) { (void)what; return 0; }
void *hos_switch2_open(int what) { (void)what; return NULL; }
int hos_switch2_is(const void *pad) { (void)pad; return 0; }
void hos_switch2_close(void *pad) { (void)pad; }
int hos_switch2_connected(void *pad) { (void)pad; return 0; }
void hos_switch2_update(void) {}
int hos_switch2_button(void *pad, int button) { (void)pad, (void)button; return 0; }
int16_t hos_switch2_axis(void *pad, int axis) { (void)pad, (void)axis; return 0; }
const char *hos_switch2_name(void *pad) { (void)pad; return "?"; }
int32_t hos_switch2_id(void *pad) { (void)pad; return 0; }
void hos_switch2_rumble(void *pad, uint16_t low, uint16_t high) { (void)pad, (void)low, (void)high; }
int hos_switch2_sensor(void *pad, int type, float *data, int n) { (void)pad, (void)type, (void)data, (void)n; return 0; }
#endif
