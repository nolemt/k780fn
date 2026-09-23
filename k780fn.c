/*
 * k780fn.c
 *
 * Set Logitech K780 top-row behavior through a Unifying receiver on Windows,
 * without Logitech Options and without hidapi.dll.
 *
 * Commands:
 *   k780fn.exe status   - show current Fn-swap state
 *   k780fn.exe fkeys    - F1..F12 work directly; hold Fn for special/media keys
 *   k780fn.exe media    - special/media keys work directly; hold Fn for F1..F12
 *
 * Target:
 *   Logitech Unifying Receiver VID 046D, PID C52B
 *   Logitech K780 WPID 405B
 *
 * Build with MSVC:
 *   cl /nologo /W4 /O2 /DUNICODE /D_UNICODE k780fn.c ^
 *      /link setupapi.lib hid.lib /out:k780fn.exe
 *
 * Build with MinGW-w64:
 *   gcc -std=c11 -O2 -Wall -Wextra k780fn.c -o k780fn.exe -lsetupapi -lhid
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _MSC_VER
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#endif

#define LOGITECH_VID       0x046D
#define UNIFYING_PID       0xC52B

#define LOGITECH_USAGE_PAGE 0xFF00
#define HIDPP_SHORT_USAGE   0x0001
#define HIDPP_LONG_USAGE    0x0002

#define K780_WPID          0x405B

#define HIDPP_SHORT_ID     0x10
#define HIDPP_LONG_ID      0x11
#define HIDPP_RECEIVER     0xFF

/* Use an application software ID that is not 0.
 * 0 is normally reserved for notifications.
 * 0x0B is also used by modern Solaar; choosing it here is harmless when
 * Solaar is not running, but to avoid collisions we use 0x0E.
 */
#define SOFTWARE_ID        0x0E

#define FEATURE_ROOT             0x0000
#define FEATURE_FN_INVERSION     0x40A0
#define FEATURE_NEW_FN_INVERSION 0x40A2

#define MAX_REPORT_SIZE    64
#define PATH_CHARS         1024

typedef struct HidEndpoint {
    wchar_t path[PATH_CHARS];
    USHORT input_len;
    USHORT output_len;
    USHORT feature_len;
    USHORT usage_page;
    USHORT usage;
} HidEndpoint;

typedef enum IoResult {
    IO_ERROR = -1,
    IO_TIMEOUT = 0,
    IO_OK = 1
} IoResult;

static void print_win_error(const char *what)
{
    DWORD err = GetLastError();
    char *msg = NULL;

    FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&msg,
        0,
        NULL
    );

    if (msg) {
        fprintf(stderr, "%s failed: Win32 error %lu: %s", what,
                (unsigned long)err, msg);
        LocalFree(msg);
    } else {
        fprintf(stderr, "%s failed: Win32 error %lu\n",
                what, (unsigned long)err);
    }
}

static void dump_report(const char *prefix, const unsigned char *buf, size_t n)
{
    size_t i;
    printf("%s", prefix);
    for (i = 0; i < n; ++i) {
        printf("%02X", buf[i]);
        if (i + 1 != n)
            putchar(' ');
    }
    putchar('\n');
}

static bool find_endpoint(USHORT wanted_usage, HidEndpoint *out)
{
    GUID hid_guid;
    HDEVINFO dev_info = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA if_data;
    DWORD index = 0;
    bool found = false;

    memset(out, 0, sizeof(*out));

    HidD_GetHidGuid(&hid_guid);

    dev_info = SetupDiGetClassDevsW(
        &hid_guid,
        NULL,
        NULL,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );

    if (dev_info == INVALID_HANDLE_VALUE) {
        print_win_error("SetupDiGetClassDevsW");
        return false;
    }

    memset(&if_data, 0, sizeof(if_data));
    if_data.cbSize = sizeof(if_data);

    while (SetupDiEnumDeviceInterfaces(
               dev_info, NULL, &hid_guid, index++, &if_data)) {

        DWORD required = 0;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail = NULL;
        HANDLE h = INVALID_HANDLE_VALUE;
        HIDD_ATTRIBUTES attr;
        PHIDP_PREPARSED_DATA ppd = NULL;
        HIDP_CAPS caps;
        NTSTATUS st;

        SetupDiGetDeviceInterfaceDetailW(
            dev_info, &if_data, NULL, 0, &required, NULL
        );

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || required == 0)
            continue;

        detail = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)malloc(required);
        if (!detail)
            break;

        memset(detail, 0, required);
        detail->cbSize = sizeof(*detail);

        if (!SetupDiGetDeviceInterfaceDetailW(
                dev_info, &if_data, detail, required, NULL, NULL)) {
            free(detail);
            continue;
        }

        /*
         * Access 0 is enough to inspect attributes and report descriptor.
         * We reopen the selected path with read/write access later.
         */
        h = CreateFileW(
            detail->DevicePath,
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );

        if (h == INVALID_HANDLE_VALUE) {
            free(detail);
            continue;
        }

        memset(&attr, 0, sizeof(attr));
        attr.Size = sizeof(attr);

        if (!HidD_GetAttributes(h, &attr) ||
            attr.VendorID != LOGITECH_VID ||
            attr.ProductID != UNIFYING_PID) {
            CloseHandle(h);
            free(detail);
            continue;
        }

        if (!HidD_GetPreparsedData(h, &ppd)) {
            CloseHandle(h);
            free(detail);
            continue;
        }

        memset(&caps, 0, sizeof(caps));
        st = HidP_GetCaps(ppd, &caps);
        HidD_FreePreparsedData(ppd);

        if (st != HIDP_STATUS_SUCCESS) {
            CloseHandle(h);
            free(detail);
            continue;
        }

        if (caps.UsagePage == LOGITECH_USAGE_PAGE &&
            caps.Usage == wanted_usage) {

            wcsncpy(out->path, detail->DevicePath, PATH_CHARS - 1);
            out->path[PATH_CHARS - 1] = L'\0';

            out->input_len = caps.InputReportByteLength;
            out->output_len = caps.OutputReportByteLength;
            out->feature_len = caps.FeatureReportByteLength;
            out->usage_page = caps.UsagePage;
            out->usage = caps.Usage;

            found = true;

            CloseHandle(h);
            free(detail);
            break;
        }

        CloseHandle(h);
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return found;
}

static HANDLE open_endpoint(const HidEndpoint *ep)
{
    return CreateFileW(
        ep->path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL
    );
}

static IoResult read_timeout(HANDLE h,
                             unsigned char *buf,
                             DWORD len,
                             DWORD timeout_ms,
                             DWORD *bytes_read)
{
    OVERLAPPED ov;
    DWORD transferred = 0;
    BOOL ok;
    DWORD err;
    DWORD wait_rc;

    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) {
        print_win_error("CreateEventW");
        return IO_ERROR;
    }

    memset(buf, 0, len);

    ok = ReadFile(h, buf, len, &transferred, &ov);

    if (ok) {
        if (bytes_read)
            *bytes_read = transferred;
        CloseHandle(ov.hEvent);
        return IO_OK;
    }

    err = GetLastError();

    if (err != ERROR_IO_PENDING) {
        SetLastError(err);
        print_win_error("ReadFile");
        CloseHandle(ov.hEvent);
        return IO_ERROR;
    }

    wait_rc = WaitForSingleObject(ov.hEvent, timeout_ms);

    if (wait_rc == WAIT_TIMEOUT) {
        CancelIoEx(h, &ov);
        WaitForSingleObject(ov.hEvent, INFINITE);
        CloseHandle(ov.hEvent);
        return IO_TIMEOUT;
    }

    if (wait_rc != WAIT_OBJECT_0) {
        print_win_error("WaitForSingleObject");
        CancelIoEx(h, &ov);
        CloseHandle(ov.hEvent);
        return IO_ERROR;
    }

    if (!GetOverlappedResult(h, &ov, &transferred, FALSE)) {
        print_win_error("GetOverlappedResult(read)");
        CloseHandle(ov.hEvent);
        return IO_ERROR;
    }

    if (bytes_read)
        *bytes_read = transferred;

    CloseHandle(ov.hEvent);
    return IO_OK;
}

static IoResult write_timeout(HANDLE h,
                              const unsigned char *buf,
                              DWORD len,
                              DWORD timeout_ms,
                              DWORD *bytes_written)
{
    OVERLAPPED ov;
    DWORD transferred = 0;
    BOOL ok;
    DWORD err;
    DWORD wait_rc;

    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) {
        print_win_error("CreateEventW");
        return IO_ERROR;
    }

    ok = WriteFile(h, buf, len, &transferred, &ov);

    if (ok) {
        if (bytes_written)
            *bytes_written = transferred;
        CloseHandle(ov.hEvent);
        return IO_OK;
    }

    err = GetLastError();

    if (err != ERROR_IO_PENDING) {
        SetLastError(err);
        print_win_error("WriteFile");
        CloseHandle(ov.hEvent);
        return IO_ERROR;
    }

    wait_rc = WaitForSingleObject(ov.hEvent, timeout_ms);

    if (wait_rc == WAIT_TIMEOUT) {
        CancelIoEx(h, &ov);
        WaitForSingleObject(ov.hEvent, INFINITE);
        CloseHandle(ov.hEvent);
        return IO_TIMEOUT;
    }

    if (wait_rc != WAIT_OBJECT_0) {
        print_win_error("WaitForSingleObject");
        CancelIoEx(h, &ov);
        CloseHandle(ov.hEvent);
        return IO_ERROR;
    }

    if (!GetOverlappedResult(h, &ov, &transferred, FALSE)) {
        print_win_error("GetOverlappedResult(write)");
        CloseHandle(ov.hEvent);
        return IO_ERROR;
    }

    if (bytes_written)
        *bytes_written = transferred;

    CloseHandle(ov.hEvent);
    return IO_OK;
}

static void drain_endpoint(HANDLE h, USHORT input_len)
{
    unsigned char buf[MAX_REPORT_SIZE];
    DWORD n = 0;
    int i;

    if (input_len == 0 || input_len > MAX_REPORT_SIZE)
        return;

    /*
     * Do not drain forever if notifications keep arriving.
     * A few non-blocking-ish reads are enough to discard stale replies.
     */
    for (i = 0; i < 8; ++i) {
        IoResult r = read_timeout(h, buf, input_len, 5, &n);
        if (r != IO_OK)
            break;
    }
}

static bool send_short(HANDLE h,
                       const HidEndpoint *ep,
                       const unsigned char packet[7])
{
    unsigned char out[MAX_REPORT_SIZE];
    DWORD n = 0;
    IoResult r;

    if (ep->output_len < 7 || ep->output_len > MAX_REPORT_SIZE) {
        fprintf(stderr,
                "Unexpected short endpoint output report length: %u\n",
                (unsigned)ep->output_len);
        return false;
    }

    memset(out, 0, sizeof(out));
    memcpy(out, packet, 7);

    dump_report("TX: ", packet, 7);

    r = write_timeout(h, out, ep->output_len, 1000, &n);
    if (r != IO_OK) {
        if (r == IO_TIMEOUT)
            fprintf(stderr, "Write timed out.\n");
        return false;
    }

    return true;
}

/*
 * Wait for a matching HID++ 2.0 reply.
 *
 * Important on Windows/Unifying:
 * a 0x10 short request can be answered by either a 0x10 short report
 * or a 0x11 long report.  The receiver exposes these as two separate
 * top-level HID collections (Usage 0001 and 0002), so poll both.
 *
 * Expected normal reply prefix:
 *   <10|11> <slot> <feature-index> <function+software-id> ...
 *
 * HID++ 2.0 error reply prefix:
 *   <10|11> <slot> FF <original-feature-index>
 *           <original-fn+swid> <error> ...
 */
static bool wait_hidpp20_reply(HANDLE short_h,
                               const HidEndpoint *short_ep,
                               HANDLE long_h,
                               const HidEndpoint *long_ep,
                               uint8_t slot,
                               uint8_t feature_index,
                               uint8_t fn_swid,
                               unsigned char reply[MAX_REPORT_SIZE],
                               DWORD *reply_len,
                               DWORD timeout_ms)
{
    ULONGLONG start = GetTickCount64();

    if (short_ep->input_len < 7 ||
        short_ep->input_len > MAX_REPORT_SIZE ||
        long_ep->input_len < 7 ||
        long_ep->input_len > MAX_REPORT_SIZE) {
        fprintf(stderr, "Unexpected HID input report length.\n");
        return false;
    }

    for (;;) {
        HANDLE handles[2] = { short_h, long_h };
        const HidEndpoint *eps[2] = { short_ep, long_ep };
        int which;

        if (GetTickCount64() - start >= timeout_ms)
            return false;

        for (which = 0; which < 2; ++which) {
            unsigned char buf[MAX_REPORT_SIZE];
            DWORD got = 0;
            ULONGLONG elapsed = GetTickCount64() - start;
            DWORD remaining;
            DWORD slice;
            IoResult r;

            if (elapsed >= timeout_ms)
                return false;

            remaining = timeout_ms - (DWORD)elapsed;
            slice = remaining < 20 ? remaining : 20;

            r = read_timeout(handles[which],
                             buf,
                             eps[which]->input_len,
                             slice,
                             &got);

            if (r == IO_ERROR)
                return false;

            if (r == IO_TIMEOUT)
                continue;

            if (got < 7)
                continue;

            dump_report("RX: ", buf, got < 20 ? got : 20);

            if (buf[0] != HIDPP_SHORT_ID &&
                buf[0] != HIDPP_LONG_ID)
                continue;

            if (buf[1] != slot)
                continue;

            if (buf[2] == 0xFF &&
                buf[3] == feature_index &&
                buf[4] == fn_swid) {
                fprintf(stderr,
                        "HID++ 2.0 error: feature=0x%02X request=0x%02X "
                        "error=0x%02X\n",
                        feature_index, fn_swid, buf[5]);
                return false;
            }

            if (buf[2] == feature_index &&
                buf[3] == fn_swid) {
                memset(reply, 0, MAX_REPORT_SIZE);
                memcpy(reply, buf,
                       got < MAX_REPORT_SIZE ? got : MAX_REPORT_SIZE);
                if (reply_len)
                    *reply_len = got;
                return true;
            }

            /* unrelated notification/reply; keep waiting */
        }
    }
}

/*
 * Read one pairing-information entry from receiver register 0x2B5.
 *
 * HID++ 1.0 request used by Unifying receivers:
 *   10 FF 83 B5 <subregister> 00 00
 *
 * PAIRING_INFORMATION starts at subregister 0x20:
 *   slot 1 -> 0x20
 *   slot 2 -> 0x21
 *   ...
 *
 * Normal long reply starts:
 *   11 FF 83 B5 <subregister> ...
 *
 * In the returned pairing record the WPID is raw reply bytes [7],[8].
 */
static bool read_pairing_wpid(HANDLE short_h,
                              const HidEndpoint *short_ep,
                              HANDLE long_h,
                              const HidEndpoint *long_ep,
                              int slot,
                              uint16_t *wpid_out)
{
    unsigned char req[7] = {
        0x10, 0xFF, 0x83, 0xB5,
        (unsigned char)(0x20 + slot - 1),
        0x00, 0x00
    };

    ULONGLONG start;
    const DWORD timeout_ms = 350;

    if (long_ep->input_len < 9 ||
        long_ep->input_len > MAX_REPORT_SIZE) {
        fprintf(stderr,
                "Unexpected long endpoint input report length: %u\n",
                (unsigned)long_ep->input_len);
        return false;
    }

    drain_endpoint(short_h, short_ep->input_len);
    drain_endpoint(long_h, long_ep->input_len);

    if (!send_short(short_h, short_ep, req))
        return false;

    start = GetTickCount64();

    for (;;) {
        unsigned char buf[MAX_REPORT_SIZE];
        DWORD got = 0;
        ULONGLONG elapsed = GetTickCount64() - start;
        DWORD remaining;
        IoResult r;

        if (elapsed >= timeout_ms)
            return false;

        remaining = timeout_ms - (DWORD)elapsed;

        r = read_timeout(long_h, buf, long_ep->input_len,
                         remaining, &got);

        if (r == IO_TIMEOUT)
            return false;
        if (r == IO_ERROR)
            return false;
        if (got < 9)
            continue;

        dump_report("RX: ", buf, got < 20 ? got : 20);

        if (buf[0] == 0x11 &&
            buf[1] == 0xFF &&
            buf[2] == 0x83 &&
            buf[3] == 0xB5 &&
            buf[4] == req[4]) {

            *wpid_out = ((uint16_t)buf[7] << 8) | buf[8];
            return true;
        }
    }
}

static int find_k780_slot(HANDLE short_h,
                          const HidEndpoint *short_ep,
                          HANDLE long_h,
                          const HidEndpoint *long_ep)
{
    int slot;

    puts("Scanning Unifying pairing slots...");

    for (slot = 1; slot <= 6; ++slot) {
        uint16_t wpid = 0;

        printf("Slot %d: ", slot);
        fflush(stdout);

        if (read_pairing_wpid(short_h, short_ep,
                              long_h, long_ep,
                              slot, &wpid)) {
            printf("WPID %04X", wpid);

            if (wpid == K780_WPID) {
                puts("  <-- K780");
                return slot;
            }

            putchar('\n');
        } else {
            puts("empty/no reply");
        }
    }

    return -1;
}

/*
 * HID++ 2.0 Root.GetFeature(feature_id)
 *
 * Root is always feature index 0.
 * Function 0 = GetFeature.
 *
 * Packet:
 *   10 <slot> 00 0E <feature_hi> <feature_lo> 00
 *
 * Reply parameter 0 is the actual feature index.
 */
static bool resolve_feature(HANDLE short_h,
                            const HidEndpoint *short_ep,
                            HANDLE long_h,
                            const HidEndpoint *long_ep,
                            uint8_t slot,
                            uint16_t feature_id,
                            uint8_t *feature_index_out)
{
    unsigned char req[7];
    unsigned char reply[MAX_REPORT_SIZE];
    DWORD reply_len = 0;

    req[0] = 0x10;
    req[1] = slot;
    req[2] = 0x00; /* Root feature index */
    req[3] = (0x00 << 4) | SOFTWARE_ID; /* function 0 */
    req[4] = (unsigned char)((feature_id >> 8) & 0xFF);
    req[5] = (unsigned char)(feature_id & 0xFF);
    req[6] = 0x00;

    drain_endpoint(short_h, short_ep->input_len);
    drain_endpoint(long_h, long_ep->input_len);

    if (!send_short(short_h, short_ep, req))
        return false;

    if (!wait_hidpp20_reply(
            short_h, short_ep,
            long_h, long_ep,
            slot, 0x00, req[3],
            reply, &reply_len, 1000)) {
        return false;
    }

    (void)reply_len;

    if (reply[4] == 0x00) {
        printf("Feature 0x%04X is not supported.\n", feature_id);
        return false;
    }

    *feature_index_out = reply[4];

    printf("Feature 0x%04X -> index 0x%02X",
           feature_id, reply[4]);

    /* reply[5] contains feature type; reply[6] is feature version */
    printf(" (type=0x%02X, version=%u)\n",
           reply[5], (unsigned)reply[6]);

    return true;
}

/*
 * NEW_FN_INVERSION / FN_INVERSION:
 *   function 0 -> read state
 *
 * Solaar's meaning:
 *   0 = direct F1..F12, hold Fn for special/media
 *   1 = direct special/media, hold Fn for F1..F12
 */
static bool read_fn_swap(HANDLE short_h,
                         const HidEndpoint *short_ep,
                         HANDLE long_h,
                         const HidEndpoint *long_ep,
                         uint8_t slot,
                         uint8_t feature_index,
                         uint8_t *state_out)
{
    unsigned char req[7] = {
        0x10,
        slot,
        feature_index,
        (0x00 << 4) | SOFTWARE_ID,
        0x00, 0x00, 0x00
    };

    unsigned char reply[MAX_REPORT_SIZE];
    DWORD reply_len = 0;

    drain_endpoint(short_h, short_ep->input_len);
    drain_endpoint(long_h, long_ep->input_len);

    if (!send_short(short_h, short_ep, req))
        return false;

    if (!wait_hidpp20_reply(
            short_h, short_ep,
            long_h, long_ep,
            slot, feature_index, req[3],
            reply, &reply_len, 1000)) {
        return false;
    }

    (void)reply_len;
    *state_out = reply[4] ? 1 : 0;
    return true;
}

/*
 * Function 1 -> write state.
 * state 0: direct F1..F12
 * state 1: direct special/media functions
 */
static bool write_fn_swap(HANDLE short_h,
                          const HidEndpoint *short_ep,
                          HANDLE long_h,
                          const HidEndpoint *long_ep,
                          uint8_t slot,
                          uint8_t feature_index,
                          uint8_t state)
{
    unsigned char req[7] = {
        0x10,
        slot,
        feature_index,
        (0x01 << 4) | SOFTWARE_ID,
        state ? 0x01 : 0x00,
        0x00,
        0x00
    };

    unsigned char reply[MAX_REPORT_SIZE];
    DWORD reply_len = 0;

    drain_endpoint(short_h, short_ep->input_len);
    drain_endpoint(long_h, long_ep->input_len);

    if (!send_short(short_h, short_ep, req))
        return false;

    if (!wait_hidpp20_reply(
            short_h, short_ep,
            long_h, long_ep,
            slot, feature_index, req[3],
            reply, &reply_len, 1000)) {
        return false;
    }

    (void)reply_len;
    return true;
}

static void print_usage(const char *exe)
{
    printf("Usage:\n");
    printf("  %s status   Show current mode\n", exe);
    printf("  %s fkeys    F1..F12 directly; Fn = special/media\n", exe);
    printf("  %s media    Special/media directly; Fn = F1..F12\n", exe);
}

int main(int argc, char **argv)
{
    enum {
        CMD_STATUS,
        CMD_FKEYS,
        CMD_MEDIA
    } command;

    HidEndpoint short_ep;
    HidEndpoint long_ep;

    HANDLE short_h = INVALID_HANDLE_VALUE;
    HANDLE long_h = INVALID_HANDLE_VALUE;

    int slot;
    uint8_t fn_feature = 0;
    uint16_t fn_feature_id = 0;
    uint8_t state = 0;
    int exit_code = 1;

    if (argc != 2) {
        print_usage(argv[0]);
        return 2;
    }

    if (_stricmp(argv[1], "status") == 0) {
        command = CMD_STATUS;
    } else if (_stricmp(argv[1], "fkeys") == 0) {
        command = CMD_FKEYS;
    } else if (_stricmp(argv[1], "media") == 0) {
        command = CMD_MEDIA;
    } else {
        print_usage(argv[0]);
        return 2;
    }

    puts("Looking for Logitech Unifying Receiver 046D:C52B...");

    if (!find_endpoint(HIDPP_SHORT_USAGE, &short_ep)) {
        fprintf(stderr,
                "Could not find HID++ short endpoint "
                "(UsagePage FF00, Usage 0001).\n");
        goto cleanup;
    }

    if (!find_endpoint(HIDPP_LONG_USAGE, &long_ep)) {
        fprintf(stderr,
                "Could not find HID++ long endpoint "
                "(UsagePage FF00, Usage 0002).\n");
        goto cleanup;
    }

    printf("Short endpoint: input=%u output=%u\n",
           (unsigned)short_ep.input_len,
           (unsigned)short_ep.output_len);

    printf("Long endpoint : input=%u output=%u\n",
           (unsigned)long_ep.input_len,
           (unsigned)long_ep.output_len);

    short_h = open_endpoint(&short_ep);
    if (short_h == INVALID_HANDLE_VALUE) {
        print_win_error("CreateFileW(short HID++ endpoint)");
        fprintf(stderr,
                "If Logitech Options/Options+ or another HID++ tool is "
                "running, close it and try again.\n");
        goto cleanup;
    }

    long_h = open_endpoint(&long_ep);
    if (long_h == INVALID_HANDLE_VALUE) {
        print_win_error("CreateFileW(long HID++ endpoint)");
        goto cleanup;
    }

    slot = find_k780_slot(short_h, &short_ep,
                          long_h, &long_ep);

    if (slot < 1) {
        fprintf(stderr,
                "\nK780 (WPID 405B) was not found in Unifying slots 1..6.\n"
                "Make sure this K780 is paired to this receiver.\n");
        goto cleanup;
    }

    printf("\nK780 is paired in receiver slot %d.\n", slot);

    /*
     * K780 is known to expose NEW_FN_INVERSION (0x40A2).
     * Try it first. 0x40A0 is retained as a conservative fallback for
     * firmware variants that expose the older Fn Inversion feature.
     */
    if (resolve_feature(short_h, &short_ep,
                        long_h, &long_ep,
                        (uint8_t)slot,
                        FEATURE_NEW_FN_INVERSION, &fn_feature)) {
        fn_feature_id = FEATURE_NEW_FN_INVERSION;
    } else {
        puts("Trying legacy FN_INVERSION feature 0x40A0...");
        if (resolve_feature(short_h, &short_ep,
                            long_h, &long_ep,
                            (uint8_t)slot,
                            FEATURE_FN_INVERSION, &fn_feature)) {
            fn_feature_id = FEATURE_FN_INVERSION;
        } else {
            fprintf(stderr,
                    "Neither 0x40A2 nor 0x40A0 could be resolved.\n"
                    "Press a key to wake the K780 and try again.\n");
            goto cleanup;
        }
    }

    printf("Using Fn feature 0x%04X at index 0x%02X.\n",
           fn_feature_id, fn_feature);

    if (!read_fn_swap(short_h, &short_ep,
                      long_h, &long_ep,
                      (uint8_t)slot, fn_feature, &state)) {
        fprintf(stderr,
                "Could not read Fn-swap state. "
                "Press a key to wake the keyboard and retry.\n");
        goto cleanup;
    }

    printf("Current mode: %s\n",
           state
               ? "special/media keys directly (Fn-swap = 1)"
               : "F1..F12 directly (Fn-swap = 0)");

    if (command == CMD_STATUS) {
        exit_code = 0;
        goto cleanup;
    }

    {
        uint8_t wanted =
            (command == CMD_FKEYS) ? 0 : 1;

        if (state != wanted) {
            printf("Setting Fn-swap to %u...\n", (unsigned)wanted);

            if (!write_fn_swap(short_h, &short_ep,
                               long_h, &long_ep,
                               (uint8_t)slot,
                               fn_feature,
                               wanted)) {
                fprintf(stderr,
                        "Failed to write Fn-swap state.\n");
                goto cleanup;
            }
        } else {
            puts("Requested mode is already active.");
        }

        Sleep(80);

        if (!read_fn_swap(short_h, &short_ep,
                          long_h, &long_ep,
                          (uint8_t)slot,
                          fn_feature,
                          &state)) {
            fprintf(stderr,
                    "Write was sent, but verification read failed.\n");
            goto cleanup;
        }

        printf("Verified mode: %s\n",
               state
                   ? "special/media keys directly (Fn-swap = 1)"
                   : "F1..F12 directly (Fn-swap = 0)");

        if (state != wanted) {
            fprintf(stderr,
                    "Verification failed: device did not keep "
                    "the requested state.\n");
            goto cleanup;
        }
    }

    puts("Done.");
    exit_code = 0;

cleanup:
    if (long_h != INVALID_HANDLE_VALUE)
        CloseHandle(long_h);

    if (short_h != INVALID_HANDLE_VALUE)
        CloseHandle(short_h);

    return exit_code;
}
