/*
 * k780fn-auto.c
 * Silent Logitech K780 Fn-lock setter for 64-bit Windows.
 *
 * Behavior:
 *   - Works with Logitech K780 paired through a Unifying receiver (046D:C52B)
 *   - Finds K780 by WPID 405B
 *   - Resolves HID++ Fn Inversion feature dynamically (0x40A2, fallback 0x40A0)
 *   - Forces Fn inversion = 0, i.e. F1..F12 work directly
 *   - Retries at startup because the receiver/keyboard may not yet be ready
 *   - No console window, no GUI, no third-party DLL, no VC runtime
 *
 * This source deliberately avoids the C runtime.  The final EXE imports only
 * Windows system DLLs: KERNEL32.dll, SETUPAPI.dll and HID.dll.
 */

#define NULL ((void*)0)

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef unsigned long long uptr;
typedef void*              HANDLE;
typedef void*              HDEVINFO;
typedef u16                WCHAR;
typedef int                BOOL;
typedef long               NTSTATUS;
typedef unsigned long long ULONG_PTR;

#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(uptr)-1)

#define GENERIC_READ         0x80000000u
#define GENERIC_WRITE        0x40000000u
#define FILE_SHARE_READ      0x00000001u
#define FILE_SHARE_WRITE     0x00000002u
#define OPEN_EXISTING        3u
#define FILE_FLAG_OVERLAPPED 0x40000000u
#define WAIT_OBJECT_0        0u
#define WAIT_TIMEOUT         258u
#define INFINITE             0xFFFFFFFFu
#define ERROR_IO_PENDING     997u
#define DIGCF_PRESENT         0x00000002u
#define DIGCF_DEVICEINTERFACE 0x00000010u
#define HEAP_ZERO_MEMORY      0x00000008u

#define LOGITECH_VID 0x046D
#define UNIFYING_PID 0xC52B
#define HID_USAGE_PAGE_VENDOR 0xFF00
#define HID_USAGE_SHORT 0x0001
#define HID_USAGE_LONG  0x0002
#define K780_WPID 0x405B
#define HIDPP_SHORT_ID 0x10
#define HIDPP_LONG_ID  0x11
#define SOFTWARE_ID 0x0E
#define FEATURE_FN_INVERSION     0x40A0
#define FEATURE_NEW_FN_INVERSION 0x40A2
#define MAX_REPORT 64
#define MAX_PATH_W 1024

/* Silent process exit codes. 0 means the Fn mode was verified successfully. */
#define RC_OK             0
#define RC_NO_RECEIVER   20
#define RC_OPEN_RECEIVER 21
#define RC_NO_K780       30
#define RC_NO_FN_FEATURE 40
#define RC_READ_STATE    41
#define RC_WRITE_STATE   42
#define RC_VERIFY        43

/* Minimal Windows structures required by SetupAPI/HID. */
typedef struct GUID_ {
    u32 Data1;
    u16 Data2;
    u16 Data3;
    u8 Data4[8];
} GUID_;

typedef struct SP_DEVICE_INTERFACE_DATA_ {
    u32 cbSize;
    GUID_ InterfaceClassGuid;
    u32 Flags;
    ULONG_PTR Reserved;
} SP_DEVICE_INTERFACE_DATA_;

typedef struct SP_DEVICE_INTERFACE_DETAIL_DATA_W_ {
    u32 cbSize;
    WCHAR DevicePath[1];
} SP_DEVICE_INTERFACE_DETAIL_DATA_W_;

typedef struct HIDD_ATTRIBUTES_ {
    u32 Size;
    u16 VendorID;
    u16 ProductID;
    u16 VersionNumber;
} HIDD_ATTRIBUTES_;

typedef struct HIDP_CAPS_ {
    u16 Usage;
    u16 UsagePage;
    u16 InputReportByteLength;
    u16 OutputReportByteLength;
    u16 FeatureReportByteLength;
    u16 Reserved[17];
    u16 NumberLinkCollectionNodes;
    u16 NumberInputButtonCaps;
    u16 NumberInputValueCaps;
    u16 NumberInputDataIndices;
    u16 NumberOutputButtonCaps;
    u16 NumberOutputValueCaps;
    u16 NumberOutputDataIndices;
    u16 NumberFeatureButtonCaps;
    u16 NumberFeatureValueCaps;
    u16 NumberFeatureDataIndices;
} HIDP_CAPS_;

typedef struct OVERLAPPED_ {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union {
        struct { u32 Offset; u32 OffsetHigh; } s;
        void *Pointer;
    } u;
    HANDLE hEvent;
} OVERLAPPED_;

typedef struct HidEndpoint_ {
    WCHAR path[MAX_PATH_W];
    u16 input_len;
    u16 output_len;
    u16 feature_len;
    u16 usage_page;
    u16 usage;
} HidEndpoint_;

/* Kernel32 imports. */
__declspec(dllimport) void   ExitProcess(u32);
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void*  HeapAlloc(HANDLE,u32,uptr);
__declspec(dllimport) BOOL   HeapFree(HANDLE,u32,void*);
__declspec(dllimport) HANDLE CreateFileW(const WCHAR*,u32,u32,void*,u32,u32,HANDLE);
__declspec(dllimport) BOOL   CloseHandle(HANDLE);
__declspec(dllimport) HANDLE CreateEventW(void*,BOOL,BOOL,const WCHAR*);
__declspec(dllimport) BOOL   ReadFile(HANDLE,void*,u32,u32*,OVERLAPPED_*);
__declspec(dllimport) BOOL   WriteFile(HANDLE,const void*,u32,u32*,OVERLAPPED_*);
__declspec(dllimport) u32    WaitForSingleObject(HANDLE,u32);
__declspec(dllimport) BOOL   CancelIoEx(HANDLE,OVERLAPPED_*);
__declspec(dllimport) BOOL   GetOverlappedResult(HANDLE,OVERLAPPED_*,u32*,BOOL);
__declspec(dllimport) u64    GetTickCount64(void);
__declspec(dllimport) void   Sleep(u32);
__declspec(dllimport) u32    GetLastError(void);

/* SetupAPI imports. */
__declspec(dllimport) HDEVINFO SetupDiGetClassDevsW(const GUID_*,const WCHAR*,HANDLE,u32);
__declspec(dllimport) BOOL SetupDiEnumDeviceInterfaces(HDEVINFO,void*,const GUID_*,u32,SP_DEVICE_INTERFACE_DATA_*);
__declspec(dllimport) BOOL SetupDiGetDeviceInterfaceDetailW(HDEVINFO,SP_DEVICE_INTERFACE_DATA_*,SP_DEVICE_INTERFACE_DETAIL_DATA_W_*,u32,u32*,void*);
__declspec(dllimport) BOOL SetupDiDestroyDeviceInfoList(HDEVINFO);

/* HID imports. */
__declspec(dllimport) void HidD_GetHidGuid(GUID_*);
__declspec(dllimport) u8 HidD_GetAttributes(HANDLE,HIDD_ATTRIBUTES_*);
__declspec(dllimport) u8 HidD_GetPreparsedData(HANDLE,void**);
__declspec(dllimport) u8 HidD_FreePreparsedData(void*);
__declspec(dllimport) NTSTATUS HidP_GetCaps(void*,HIDP_CAPS_*);

static HidEndpoint_ G_short_ep;
static HidEndpoint_ G_long_ep;

/* Local implementations keep the executable independent of the C runtime. */
void *memset(void *dst, int c, uptr n)
{
    u8 *d=(u8*)dst; uptr i;
    for(i=0;i<n;i++) d[i]=(u8)c;
    return dst;
}

void *memcpy(void *dst,const void *src,uptr n)
{
    u8 *d=(u8*)dst; const u8 *s=(const u8*)src; uptr i;
    for(i=0;i<n;i++) d[i]=s[i];
    return dst;
}

static void wcopy(WCHAR *dst,const WCHAR *src,u32 cap)
{
    u32 i=0;
    if(!cap) return;
    while(i+1<cap && src[i]) { dst[i]=src[i]; i++; }
    dst[i]=0;
}

static int find_endpoint(u16 wanted_usage,HidEndpoint_ *out)
{
    GUID_ g;
    HDEVINFO di;
    SP_DEVICE_INTERFACE_DATA_ ifd;
    u32 index=0;
    HANDLE heap=GetProcessHeap();

    memset(out,0,sizeof(*out));
    HidD_GetHidGuid(&g);
    di=SetupDiGetClassDevsW(&g,NULL,NULL,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(di==INVALID_HANDLE_VALUE) return 0;

    memset(&ifd,0,sizeof(ifd));
    ifd.cbSize=sizeof(ifd);

    while(SetupDiEnumDeviceInterfaces(di,NULL,&g,index++,&ifd)) {
        u32 req=0;
        SP_DEVICE_INTERFACE_DETAIL_DATA_W_ *detail;
        HANDLE h;
        HIDD_ATTRIBUTES_ attr;
        void *ppd=NULL;
        HIDP_CAPS_ caps;

        SetupDiGetDeviceInterfaceDetailW(di,&ifd,NULL,0,&req,NULL);
        if(!req) continue;

        detail=(SP_DEVICE_INTERFACE_DETAIL_DATA_W_*)HeapAlloc(heap,HEAP_ZERO_MEMORY,req);
        if(!detail) break;

        /* sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) is 8 on x64 Windows. */
        detail->cbSize=8;

        if(!SetupDiGetDeviceInterfaceDetailW(di,&ifd,detail,req,NULL,NULL)) {
            HeapFree(heap,0,detail);
            continue;
        }

        h=CreateFileW(detail->DevicePath,0,FILE_SHARE_READ|FILE_SHARE_WRITE,
                      NULL,OPEN_EXISTING,0,NULL);
        if(h==INVALID_HANDLE_VALUE) {
            HeapFree(heap,0,detail);
            continue;
        }

        memset(&attr,0,sizeof(attr));
        attr.Size=sizeof(attr);
        if(!HidD_GetAttributes(h,&attr) ||
           attr.VendorID!=LOGITECH_VID || attr.ProductID!=UNIFYING_PID) {
            CloseHandle(h);
            HeapFree(heap,0,detail);
            continue;
        }

        if(!HidD_GetPreparsedData(h,&ppd)) {
            CloseHandle(h);
            HeapFree(heap,0,detail);
            continue;
        }

        memset(&caps,0,sizeof(caps));
        if(HidP_GetCaps(ppd,&caps)==0x00110000L &&
           caps.UsagePage==HID_USAGE_PAGE_VENDOR &&
           caps.Usage==wanted_usage) {
            wcopy(out->path,detail->DevicePath,MAX_PATH_W);
            out->input_len=caps.InputReportByteLength;
            out->output_len=caps.OutputReportByteLength;
            out->feature_len=caps.FeatureReportByteLength;
            out->usage_page=caps.UsagePage;
            out->usage=caps.Usage;
            HidD_FreePreparsedData(ppd);
            CloseHandle(h);
            HeapFree(heap,0,detail);
            SetupDiDestroyDeviceInfoList(di);
            return 1;
        }

        HidD_FreePreparsedData(ppd);
        CloseHandle(h);
        HeapFree(heap,0,detail);
    }

    SetupDiDestroyDeviceInfoList(di);
    return 0;
}

static HANDLE open_endpoint(const HidEndpoint_ *ep)
{
    return CreateFileW(ep->path,GENERIC_READ|GENERIC_WRITE,
                       FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,
                       FILE_FLAG_OVERLAPPED,NULL);
}

typedef enum IORES_ { IOERR=-1, IOTIMEOUT=0, IOOK=1 } IORES_;

static IORES_ read_timeout(HANDLE h,u8 *buf,u32 len,u32 timeout,u32 *got)
{
    OVERLAPPED_ ov;
    u32 n=0,err,wr;
    BOOL ok;

    memset(&ov,0,sizeof(ov));
    ov.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!ov.hEvent) return IOERR;

    memset(buf,0,len);
    ok=ReadFile(h,buf,len,&n,&ov);
    if(ok) {
        if(got) *got=n;
        CloseHandle(ov.hEvent);
        return IOOK;
    }

    err=GetLastError();
    if(err!=ERROR_IO_PENDING) {
        CloseHandle(ov.hEvent);
        return IOERR;
    }

    wr=WaitForSingleObject(ov.hEvent,timeout);
    if(wr==WAIT_TIMEOUT) {
        CancelIoEx(h,&ov);
        WaitForSingleObject(ov.hEvent,INFINITE);
        CloseHandle(ov.hEvent);
        return IOTIMEOUT;
    }
    if(wr!=WAIT_OBJECT_0) {
        CancelIoEx(h,&ov);
        CloseHandle(ov.hEvent);
        return IOERR;
    }
    if(!GetOverlappedResult(h,&ov,&n,FALSE)) {
        CloseHandle(ov.hEvent);
        return IOERR;
    }

    if(got) *got=n;
    CloseHandle(ov.hEvent);
    return IOOK;
}

static IORES_ write_timeout(HANDLE h,const u8 *buf,u32 len,u32 timeout,u32 *sent)
{
    OVERLAPPED_ ov;
    u32 n=0,err,wr;
    BOOL ok;

    memset(&ov,0,sizeof(ov));
    ov.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!ov.hEvent) return IOERR;

    ok=WriteFile(h,buf,len,&n,&ov);
    if(ok) {
        if(sent) *sent=n;
        CloseHandle(ov.hEvent);
        return IOOK;
    }

    err=GetLastError();
    if(err!=ERROR_IO_PENDING) {
        CloseHandle(ov.hEvent);
        return IOERR;
    }

    wr=WaitForSingleObject(ov.hEvent,timeout);
    if(wr==WAIT_TIMEOUT) {
        CancelIoEx(h,&ov);
        WaitForSingleObject(ov.hEvent,INFINITE);
        CloseHandle(ov.hEvent);
        return IOTIMEOUT;
    }
    if(wr!=WAIT_OBJECT_0) {
        CancelIoEx(h,&ov);
        CloseHandle(ov.hEvent);
        return IOERR;
    }
    if(!GetOverlappedResult(h,&ov,&n,FALSE)) {
        CloseHandle(ov.hEvent);
        return IOERR;
    }

    if(sent) *sent=n;
    CloseHandle(ov.hEvent);
    return IOOK;
}

static void drain(HANDLE h,u16 input_len)
{
    u8 buf[MAX_REPORT];
    u32 n;
    int i;
    if(input_len==0 || input_len>MAX_REPORT) return;
    for(i=0;i<6;i++) {
        if(read_timeout(h,buf,input_len,3,&n)!=IOOK) break;
    }
}

static int send_short(HANDLE h,const HidEndpoint_ *ep,const u8 packet[7])
{
    u8 out[MAX_REPORT];
    u32 n;
    if(ep->output_len<7 || ep->output_len>MAX_REPORT) return 0;
    memset(out,0,sizeof(out));
    memcpy(out,packet,7);
    return write_timeout(h,out,ep->output_len,700,&n)==IOOK;
}

static int wait_hidpp_reply(HANDLE sh,const HidEndpoint_ *se,
                            HANDLE lh,const HidEndpoint_ *le,
                            u8 slot,u8 feature,u8 fn_swid,
                            u8 reply[MAX_REPORT],u32 timeout)
{
    u64 start=GetTickCount64();

    while(GetTickCount64()-start<timeout) {
        HANDLE hs[2]={sh,lh};
        const HidEndpoint_ *eps[2]={se,le};
        int k;

        for(k=0;k<2;k++) {
            u8 buf[MAX_REPORT];
            u32 got=0;
            u64 elapsed=GetTickCount64()-start;
            u32 remaining,slice;
            IORES_ r;

            if(elapsed>=timeout) return 0;
            if(eps[k]->input_len<7 || eps[k]->input_len>MAX_REPORT) return 0;

            remaining=timeout-(u32)elapsed;
            slice=remaining<15?remaining:15;
            r=read_timeout(hs[k],buf,eps[k]->input_len,slice,&got);

            if(r==IOERR) return 0;
            if(r!=IOOK || got<7) continue;
            if(buf[0]!=HIDPP_SHORT_ID && buf[0]!=HIDPP_LONG_ID) continue;
            if(buf[1]!=slot) continue;

            /* HID++ 2.0 error reply. */
            if(buf[2]==0xFF && buf[3]==feature && buf[4]==fn_swid) return 0;

            if(buf[2]==feature && buf[3]==fn_swid) {
                memset(reply,0,MAX_REPORT);
                memcpy(reply,buf,got<MAX_REPORT?got:MAX_REPORT);
                return 1;
            }
        }
    }

    return 0;
}

static int read_pairing_wpid(HANDLE sh,const HidEndpoint_ *se,
                             HANDLE lh,const HidEndpoint_ *le,
                             int slot,u16 *wpid)
{
    u8 req[7]={0x10,0xFF,0x83,0xB5,(u8)(0x20+slot-1),0,0};
    u64 start;

    if(le->input_len<9 || le->input_len>MAX_REPORT) return 0;
    drain(sh,se->input_len);
    drain(lh,le->input_len);
    if(!send_short(sh,se,req)) return 0;

    start=GetTickCount64();
    while(GetTickCount64()-start<300) {
        u8 buf[MAX_REPORT];
        u32 got=0;
        IORES_ r=read_timeout(lh,buf,le->input_len,25,&got);
        if(r==IOERR) return 0;
        if(r!=IOOK || got<9) continue;

        if(buf[0]==0x11 && buf[1]==0xFF &&
           buf[2]==0x83 && buf[3]==0xB5 && buf[4]==req[4]) {
            *wpid=(u16)(((u16)buf[7]<<8)|buf[8]);
            return 1;
        }
    }
    return 0;
}

static int find_k780_slot(HANDLE sh,const HidEndpoint_ *se,
                          HANDLE lh,const HidEndpoint_ *le)
{
    int slot;
    for(slot=1;slot<=6;slot++) {
        u16 wpid=0;
        if(read_pairing_wpid(sh,se,lh,le,slot,&wpid) && wpid==K780_WPID)
            return slot;
    }
    return -1;
}

/* HID++ 2.0 Root.GetFeature. Root is always feature index 0. */
static int resolve_feature(HANDLE sh,const HidEndpoint_ *se,
                           HANDLE lh,const HidEndpoint_ *le,
                           u8 slot,u16 feature_id,u8 *feature_index)
{
    u8 req[7]={0x10,slot,0x00,SOFTWARE_ID,
               (u8)(feature_id>>8),(u8)feature_id,0};
    u8 reply[MAX_REPORT];

    drain(sh,se->input_len);
    drain(lh,le->input_len);
    if(!send_short(sh,se,req)) return 0;
    if(!wait_hidpp_reply(sh,se,lh,le,slot,0x00,req[3],reply,800)) return 0;
    if(reply[4]==0) return 0;
    *feature_index=reply[4];
    return 1;
}

/* Function 0: read Fn inversion. 0 means F1..F12 directly. */
static int read_fn(HANDLE sh,const HidEndpoint_ *se,
                   HANDLE lh,const HidEndpoint_ *le,
                   u8 slot,u8 index,u8 *state)
{
    u8 req[7]={0x10,slot,index,SOFTWARE_ID,0,0,0};
    u8 reply[MAX_REPORT];

    drain(sh,se->input_len);
    drain(lh,le->input_len);
    if(!send_short(sh,se,req)) return 0;
    if(!wait_hidpp_reply(sh,se,lh,le,slot,index,req[3],reply,800)) return 0;
    *state=reply[4]?1:0;
    return 1;
}

/* Function 1: write Fn inversion. */
static int write_fn(HANDLE sh,const HidEndpoint_ *se,
                    HANDLE lh,const HidEndpoint_ *le,
                    u8 slot,u8 index,u8 state)
{
    u8 req[7]={0x10,slot,index,(u8)(0x10|SOFTWARE_ID),state?1:0,0,0};
    u8 reply[MAX_REPORT];

    drain(sh,se->input_len);
    drain(lh,le->input_len);
    if(!send_short(sh,se,req)) return 0;
    return wait_hidpp_reply(sh,se,lh,le,slot,index,req[3],reply,800);
}

static int set_fkeys_once(void)
{
    HidEndpoint_ *se=&G_short_ep;
    HidEndpoint_ *le=&G_long_ep;
    HANDLE sh=INVALID_HANDLE_VALUE;
    HANDLE lh=INVALID_HANDLE_VALUE;
    int slot,rc=RC_OK;
    u8 feature_index=0,state=0;

    if(!find_endpoint(HID_USAGE_SHORT,se) || !find_endpoint(HID_USAGE_LONG,le))
        return RC_NO_RECEIVER;

    sh=open_endpoint(se);
    if(sh==INVALID_HANDLE_VALUE) return RC_OPEN_RECEIVER;

    lh=open_endpoint(le);
    if(lh==INVALID_HANDLE_VALUE) {
        CloseHandle(sh);
        return RC_OPEN_RECEIVER;
    }

    slot=find_k780_slot(sh,se,lh,le);
    if(slot<1) {
        rc=RC_NO_K780;
        goto done;
    }

    if(!resolve_feature(sh,se,lh,le,(u8)slot,
                        FEATURE_NEW_FN_INVERSION,&feature_index)) {
        if(!resolve_feature(sh,se,lh,le,(u8)slot,
                            FEATURE_FN_INVERSION,&feature_index)) {
            rc=RC_NO_FN_FEATURE;
            goto done;
        }
    }

    if(!read_fn(sh,se,lh,le,(u8)slot,feature_index,&state)) {
        rc=RC_READ_STATE;
        goto done;
    }

    if(state!=0) {
        if(!write_fn(sh,se,lh,le,(u8)slot,feature_index,0)) {
            rc=RC_WRITE_STATE;
            goto done;
        }
        Sleep(60);
    }

    if(!read_fn(sh,se,lh,le,(u8)slot,feature_index,&state) || state!=0) {
        rc=RC_VERIFY;
        goto done;
    }

    rc=RC_OK;

done:
    CloseHandle(lh);
    CloseHandle(sh);
    return rc;
}

/*
 * GUI-subsystem PE entry point: no console window is allocated.
 * Retry silently to tolerate receiver initialization / sleeping keyboard.
 */
__attribute__((noreturn)) void entry(void)
{
    int rc=RC_NO_RECEIVER;
    int attempt;

    for(attempt=0;attempt<5;attempt++) {
        rc=set_fkeys_once();
        if(rc==RC_OK) break;
        Sleep(1600);
    }

    ExitProcess((u32)rc);
    for(;;) { }
}
