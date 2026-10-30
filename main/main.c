#include <stdbool.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "cJSON.h"
#include "esp_event.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "usb/usb_host.h"
#include "wifi_credentials.h"

#define USB_DESC_TYPE_CONFIGURATION 0x02
#define USB_DESC_TYPE_INTERFACE     0x04
#define USB_DESC_TYPE_ENDPOINT      0x05
#define USB_CLASS_PRINTER           0x07
#define USB_CLASS_VENDOR_SPECIFIC   0xFF
#define USB_ENDPOINT_DIR_IN          0x80
#define USB_ENDPOINT_XFER_MASK       0x03
#define USB_ENDPOINT_XFER_CONTROL    0x00
#define USB_ENDPOINT_XFER_ISOC       0x01
#define USB_ENDPOINT_XFER_BULK       0x02
#define USB_ENDPOINT_XFER_INT        0x03
#define USB_EVENT_QUEUE_LENGTH       8

// Label dimensions and gap are millimetres; adjust for the installed media.
#define LABEL_WIDTH_MM          50
#define LABEL_HEIGHT_MM         25
#define LABEL_GAP_MM             3
#define PRINTER_INTERFACE_NUMBER 0
#define PRINTER_ALT_SETTING      0
#define BULK_OUT_MPS            64

#define MAX_LABEL_TEXT_CHARS    30
#define MAX_PRINT_PAYLOAD       256
#define HTTP_MAX_BODY_BYTES     512
#define PRINT_QUEUE_LENGTH      4
#define JOB_RECORD_COUNT        8
#define USB_JOB_POLL_TICKS      pdMS_TO_TICKS(20)
#define RAW_TCP_PORT            9100
#define RAW_CHUNK_SIZE          512
#define RAW_IDLE_TIMEOUT_MS     30000
#define RAW_QUEUE_WAIT_TICKS    pdMS_TO_TICKS(100)
#define WIFI_CONFIG_NAMESPACE   "wifi_cfg"
#define WIFI_SSID_KEY           "ssid"
#define WIFI_PASSWORD_KEY       "password"
#define WIFI_CONNECT_TIMEOUT_MS 20000
#define WIFI_SETUP_AP_PREFIX    "TSC-Setup-"

static const char *TAG = "tsc_http";
static const char *PRINTER_MODEL = "TSC TTP-244 Pro";

typedef enum {
    JOB_UNUSED = 0,
    JOB_QUEUED,
    JOB_SENDING,
    JOB_USB_TRANSFERRED,
    JOB_FAILED,
} job_status_t;

typedef struct {
    bool used;
    uint32_t id;
    job_status_t status;
    char payload[MAX_PRINT_PAYLOAD];
    size_t payload_length;
    char error[96];
} job_record_t;

typedef enum {
    PRINT_ITEM_HTTP_JOB = 0,
    PRINT_ITEM_RAW_BEGIN,
    PRINT_ITEM_RAW_DATA,
    PRINT_ITEM_RAW_END,
    PRINT_ITEM_RAW_ABORT,
} print_item_type_t;

typedef struct {
    print_item_type_t type;
    uint8_t slot;
    uint32_t id;
    uint32_t stream_id;
    size_t length;
    uint8_t data[RAW_CHUNK_SIZE];
} print_item_t;

typedef enum {
    TRANSFER_NONE = 0,
    TRANSFER_HTTP_JOB,
    TRANSFER_RAW_DATA,
} active_transfer_type_t;

typedef struct {
    usb_host_client_event_t event;
    uint8_t address;
    usb_device_handle_t handle;
} device_event_t;

static QueueHandle_t device_event_queue;
static QueueHandle_t print_job_queue;
static SemaphoreHandle_t state_mutex;
static SemaphoreHandle_t print_source_mutex;
static job_record_t jobs[JOB_RECORD_COUNT];
static uint8_t next_job_slot;
static uint32_t next_job_id = 1;
static bool current_job_active;
static uint8_t current_job_slot;
static active_transfer_type_t active_transfer_type;
static bool usb_raw_stream_active;
static bool usb_raw_stream_failed;
static uint32_t usb_raw_stream_id;
static uint64_t usb_raw_stream_queued_bytes;
static uint64_t usb_raw_stream_transferred_bytes;
static uint8_t raw_pending_data[RAW_CHUNK_SIZE];
static size_t raw_pending_length;
static size_t raw_pending_offset;

static bool wifi_connected;
static bool wifi_setup_mode;
static bool printer_connected;
static bool printer_ready_for_transfer;
static esp_netif_ip_info_t wifi_ip_info;
static bool raw_tcp_listening;
static bool raw_tcp_client_connected;
static bool raw_tcp_stream_pending;
static bool raw_tcp_stream_failed;
static uint32_t raw_tcp_stream_id;
static uint32_t next_raw_stream_id = 1;
static uint64_t raw_tcp_received_bytes;
static uint64_t raw_tcp_queued_bytes;
static uint64_t raw_tcp_usb_bytes;
static uint64_t raw_tcp_dropped_bytes;
static uint32_t raw_tcp_jobs_completed;
static uint32_t raw_tcp_jobs_failed;
static char raw_tcp_last_error[96];

static usb_host_client_handle_t client_handle;
static usb_device_handle_t open_device;
static uint8_t open_device_address;
static uint8_t bulk_out_endpoint;
static bool interface_claimed;
static bool device_gone;
static bool pending_new_device;
static uint8_t pending_device_address;
static usb_transfer_t *print_transfer;
static bool print_transfer_complete;
static usb_transfer_status_t print_transfer_status;
static int print_transfer_actual_bytes;
static int print_transfer_submitted_bytes;
static httpd_handle_t http_server;
static esp_netif_t *wifi_sta_netif;
static esp_netif_t *wifi_ap_netif;
static char configured_wifi_ssid[33];
static char configured_wifi_password[65];
static TaskHandle_t wifi_connect_waiter;
#define WIFI_MANAGER_EVENT_CONNECTED       (1U << 0)
#define WIFI_MANAGER_EVENT_DISCONNECTED    (1U << 1)
#define WIFI_MANAGER_EVENT_NEW_CREDENTIALS (1U << 2)

static const char INDEX_HTML[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>TSC Label Printer</title><style>body{font:16px system-ui;max-width:36rem;margin:2rem auto;padding:0 1rem;color:#172033}"
    "input,button{box-sizing:border-box;width:100%;padding:.8rem;margin:.35rem 0;font:inherit}button{background:#1459c7;color:white;border:0;border-radius:.4rem}"
    ".card{padding:1rem;border:1px solid #ccd3df;border-radius:.6rem;margin:1rem 0}pre{white-space:pre-wrap}</style></head><body>"
    "<h1>TSC label printer</h1><section class=card><strong>Printer:</strong> <span id=status>checking…</span><br>"
    "<strong>Wi-Fi:</strong> <span id=wifi>checking...</span> <a href='/wifi'>Setup</a><br>"
    "<strong>RAW TCP 9100:</strong> <span id=tcp>checking…</span><br><span id=counters></span></section>"
    "<section class=card><label for=key>X-API-Key</label><input id=key type=password autocomplete=off placeholder='Enter local API key'>"
    "<label for=text>Label text (up to 30 basic ASCII characters)</label><input id=text maxlength=30 value='Hello Tech Panda'>"
    "<button id=print>Print label</button><button id=test>Print test label</button><pre id=result aria-live=polite></pre></section>"
    "<script>const $=id=>document.getElementById(id),key=()=>$(\"key\").value;"
    "async function refresh(){try{let r=await fetch('/api/status'),s=await r.json();$(\"status\").textContent=s.printer_connected?(s.printer_ready_for_transfer?'Connected; USB transfer available':'Connected; USB interface not ready'):'Disconnected';"
    "$(\"tcp\").textContent=s.raw_tcp_listening?(s.raw_tcp_client_connected?'Client connected':'Listening'):'Unavailable';"
    "$(\"wifi\").textContent=s.wifi_setup_mode?('Setup AP '+s.wifi_ap_ssid+' at '+s.wifi_ap_ip):(s.wifi_connected?('Connected; '+s.wifi_ip):'Disconnected');"
    "$(\"counters\").textContent='RAW received '+s.raw_tcp_received_bytes+' B; queued '+s.raw_tcp_queued_bytes+' B; USB transferred '+s.raw_tcp_usb_bytes+' B; dropped '+s.raw_tcp_dropped_bytes+' B; jobs '+s.raw_tcp_jobs_completed+' done / '+s.raw_tcp_jobs_failed+' failed';}"
    "catch(e){$(\"status\").textContent='Status unavailable';}}"
    "async function send(path,body){let k=key();if(!k){$(\"result\").textContent='Enter the API key first.';return;}try{let r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json','X-API-Key':k},body:JSON.stringify(body||{})});let d=await r.json();"
    "if(!r.ok){$(\"result\").textContent=d.error||('Request failed: '+r.status);return;}$(\"result\").textContent='Job '+d.job_id+': '+d.status;"
    "let id=d.job_id;for(let i=0;i<20;i++){await new Promise(x=>setTimeout(x,1000));let q=await fetch('/api/jobs/'+id),j=await q.json();$(\"result\").textContent='Job '+id+': '+j.status+(j.error?' — '+j.error:'');if(['usb_transferred','failed'].includes(j.status))break;}}"
    "catch(e){$(\"result\").textContent='Network error: '+e;}}"
    "$(\"print\").onclick=()=>send('/api/print',{text:$(\"text\").value});$(\"test\").onclick=()=>send('/api/print-test',{});refresh();setInterval(refresh,3000);</script></body></html>";

static const char WIFI_SETUP_HTML[] =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'><title>Wi-Fi Setup</title>"
    "<style>body{font:16px system-ui;max-width:34rem;margin:2rem auto;padding:0 1rem;color:#172033}input,button{box-sizing:border-box;width:100%;padding:.8rem;margin:.4rem 0;font:inherit}button{background:#1459c7;color:white;border:0;border-radius:.4rem}.card{padding:1rem;border:1px solid #ccd3df;border-radius:.6rem}pre{white-space:pre-wrap}</style></head><body>"
    "<h1>Printer Wi-Fi setup</h1><section class=card><p>Enter the 2.4 GHz Wi-Fi name and password. Credentials are saved on the printer and are never displayed back.</p>"
    "<form id=f><label>Wi-Fi name (SSID)</label><input id=s maxlength=32 required autocomplete=off><label>Wi-Fi password</label><input id=p type=password maxlength=64 autocomplete=new-password><button>Save and connect</button></form><pre id=r></pre><a href='/'>Back to printer</a></section>"
    "<script>document.getElementById('f').onsubmit=async e=>{e.preventDefault();let r=document.getElementById('r');r.textContent='Saving and connecting...';try{let q=await fetch('/wifi/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:document.getElementById('s').value,password:document.getElementById('p').value})});let d=await q.json();r.textContent=d.message||d.error||('Request failed '+q.status);}catch(x){r.textContent='The connection attempt started. Reconnect to the printer setup Wi-Fi if this page stops responding.';}};</script></body></html>";

static const char *job_status_name(job_status_t status)
{
    switch (status) {
    case JOB_QUEUED: return "queued";
    case JOB_SENDING: return "sending";
    case JOB_USB_TRANSFERRED: return "usb_transferred";
    case JOB_FAILED: return "failed";
    default: return "unknown";
    }
}

static void set_printer_state(bool connected, bool ready)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    printer_connected = connected;
    printer_ready_for_transfer = ready;
    xSemaphoreGive(state_mutex);
}

static bool printer_ready_snapshot(void)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    bool ready = printer_connected && printer_ready_for_transfer;
    xSemaphoreGive(state_mutex);
    return ready;
}

static void set_raw_listener_state(bool listening)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    raw_tcp_listening = listening;
    xSemaphoreGive(state_mutex);
}

static uint32_t begin_raw_stream_state(void)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    uint32_t id = next_raw_stream_id++;
    if (next_raw_stream_id == 0) next_raw_stream_id = 1;
    raw_tcp_stream_id = id;
    raw_tcp_stream_failed = false;
    raw_tcp_client_connected = true;
    raw_tcp_stream_pending = true;
    raw_tcp_last_error[0] = '\0';
    xSemaphoreGive(state_mutex);
    return id;
}

static void set_raw_client_connected(bool connected)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    raw_tcp_client_connected = connected;
    xSemaphoreGive(state_mutex);
}

static void mark_raw_stream_failed(uint32_t stream_id, const char *reason)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (stream_id == raw_tcp_stream_id) raw_tcp_stream_failed = true;
    snprintf(raw_tcp_last_error, sizeof(raw_tcp_last_error), "%s", reason);
    xSemaphoreGive(state_mutex);
    ESP_LOGE(TAG, "RAW TCP stream %lu failed: %s", (unsigned long)stream_id, reason);
}

static bool raw_stream_failed_snapshot(uint32_t stream_id)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    bool failed = stream_id == raw_tcp_stream_id && raw_tcp_stream_failed;
    xSemaphoreGive(state_mutex);
    return failed;
}

static void add_raw_counter(uint64_t *counter, size_t amount)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    *counter += amount;
    xSemaphoreGive(state_mutex);
}

static void set_job_state(uint8_t slot, job_status_t status, const char *error)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (slot < JOB_RECORD_COUNT && jobs[slot].used) {
        jobs[slot].status = status;
        if (error != NULL) {
            snprintf(jobs[slot].error, sizeof(jobs[slot].error), "%s", error);
        }
    }
    xSemaphoreGive(state_mutex);
}

static const char *transfer_status_name(usb_transfer_status_t status)
{
    switch (status) {
    case USB_TRANSFER_STATUS_COMPLETED: return "COMPLETED";
    case USB_TRANSFER_STATUS_ERROR: return "ERROR";
    case USB_TRANSFER_STATUS_TIMED_OUT: return "TIMED_OUT";
    case USB_TRANSFER_STATUS_CANCELED: return "CANCELED";
    case USB_TRANSFER_STATUS_STALL: return "STALL";
    case USB_TRANSFER_STATUS_OVERFLOW: return "OVERFLOW";
    case USB_TRANSFER_STATUS_NO_DEVICE: return "NO_DEVICE";
    default: return "OTHER";
    }
}

static void client_event_callback(const usb_host_client_event_msg_t *event, void *arg)
{
    (void)arg;
    device_event_t message = { .event = event->event };
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        message.address = event->new_dev.address;
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        message.handle = event->dev_gone.dev_hdl;
    } else {
        return;
    }
    xQueueSend(device_event_queue, &message, portMAX_DELAY);
}

static const char *transfer_type_name(uint8_t attributes)
{
    switch (attributes & USB_ENDPOINT_XFER_MASK) {
    case USB_ENDPOINT_XFER_CONTROL: return "Control";
    case USB_ENDPOINT_XFER_ISOC: return "Isochronous";
    case USB_ENDPOINT_XFER_BULK: return "Bulk";
    case USB_ENDPOINT_XFER_INT: return "Interrupt";
    default: return "Unknown";
    }
}

static bool print_active_configuration(usb_device_handle_t device, uint8_t *bulk_out_address)
{
    const usb_config_desc_t *config = NULL;
    esp_err_t err = usb_host_get_active_config_descriptor(device, &config);
    if (err != ESP_OK || config == NULL) {
        ESP_LOGE(TAG, "Could not get active configuration descriptor: %s", esp_err_to_name(err));
        return false;
    }

    const uint8_t *bytes = (const uint8_t *)config;
    const size_t total_length = config->wTotalLength;
    ESP_LOGI(TAG, "Active USB configuration length=%u", (unsigned)total_length);
    bool printer_class_found = false;
    uint8_t current_interface = 0xFF;
    uint8_t current_alternate = 0xFF;
    uint8_t printer_bulk_out = 0;

    for (size_t offset = 0; offset + 2 <= total_length;) {
        uint8_t descriptor_length = bytes[offset];
        uint8_t descriptor_type = bytes[offset + 1];
        if (descriptor_length < 2 || offset + descriptor_length > total_length) {
            ESP_LOGE(TAG, "Malformed USB descriptor at offset %u", (unsigned)offset);
            break;
        }
        if (descriptor_type == USB_DESC_TYPE_CONFIGURATION && descriptor_length >= 9) {
            ESP_LOGI(TAG, "CONFIG value=%u interfaces=%u attributes=0x%02x max-power=%u (2mA units)",
                     bytes[offset + 5], bytes[offset + 4], bytes[offset + 7], bytes[offset + 8]);
        } else if (descriptor_type == USB_DESC_TYPE_INTERFACE && descriptor_length >= 9) {
            current_interface = bytes[offset + 2];
            current_alternate = bytes[offset + 3];
            uint8_t class_code = bytes[offset + 5];
            ESP_LOGI(TAG, "INTERFACE %u alt %u: class=0x%02x subclass=0x%02x protocol=0x%02x endpoints=%u",
                     current_interface, current_alternate, class_code, bytes[offset + 6],
                     bytes[offset + 7], bytes[offset + 4]);
            if (class_code == USB_CLASS_PRINTER) {
                printer_class_found = true;
                ESP_LOGI(TAG, "USB Printer Class 0x07 detected");
            } else if (class_code == USB_CLASS_VENDOR_SPECIFIC) {
                ESP_LOGI(TAG, "Vendor-specific interface 0xFF detected");
            }
        } else if (descriptor_type == USB_DESC_TYPE_ENDPOINT && descriptor_length >= 7) {
            uint8_t endpoint_address = bytes[offset + 2];
            uint8_t attributes = bytes[offset + 3];
            uint16_t packet_size = (uint16_t)bytes[offset + 4] | ((uint16_t)bytes[offset + 5] << 8);
            bool input = (endpoint_address & USB_ENDPOINT_DIR_IN) != 0;
            ESP_LOGI(TAG, "ENDPOINT interface %u addr=0x%02x type=%s dir=%s mps=%u",
                     current_interface, endpoint_address, transfer_type_name(attributes),
                     input ? "IN" : "OUT", packet_size & 0x07ff);
            if (!input && (attributes & USB_ENDPOINT_XFER_MASK) == USB_ENDPOINT_XFER_BULK &&
                current_interface == PRINTER_INTERFACE_NUMBER && current_alternate == PRINTER_ALT_SETTING) {
                printer_bulk_out = endpoint_address;
                ESP_LOGI(TAG, "Printer Bulk OUT endpoint discovered at 0x%02x", printer_bulk_out);
            }
        }
        offset += descriptor_length;
    }
    ESP_LOGI(TAG, "Printer-class interface present: %s", printer_class_found ? "yes" : "no");
    *bulk_out_address = printer_bulk_out;
    return printer_bulk_out != 0;
}

static void print_transfer_callback(usb_transfer_t *transfer)
{
    print_transfer_status = transfer->status;
    print_transfer_actual_bytes = transfer->actual_num_bytes;
    print_transfer_complete = true;
    if (active_transfer_type == TRANSFER_RAW_DATA) {
        ESP_LOGD(TAG, "RAW USB Bulk OUT completion: status=%s actual=%d submitted=%d",
                 transfer_status_name(transfer->status), transfer->actual_num_bytes, transfer->num_bytes);
    } else {
        ESP_LOGI(TAG, "USB Bulk OUT completion: status=%s actual=%d submitted=%d",
                 transfer_status_name(transfer->status), transfer->actual_num_bytes, transfer->num_bytes);
    }
}

static int build_tspl_payload(const char *text, char *output, size_t output_size)
{
    return snprintf(output, output_size,
        "SIZE %d mm,%d mm\r\n"
        "GAP %d mm,0 mm\r\n"
        "SPEED 2\r\n"
        "DENSITY 8\r\n"
        "DIRECTION 1\r\n"
        "CLS\r\n"
        "TEXT 32,32,\"2\",0,1,1,\"%s\"\r\n"
        "PRINT 1,1\r\n",
        LABEL_WIDTH_MM, LABEL_HEIGHT_MM, LABEL_GAP_MM, text);
}

static bool valid_label_text(const char *text)
{
    size_t length = strlen(text);
    if (length == 0 || length > MAX_LABEL_TEXT_CHARS) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)text[i];
        // Basic ASCII only; reject TSPL quotes, escape characters and controls.
        if (ch < 0x20 || ch > 0x7e || ch == '"' || ch == '\\') {
            return false;
        }
    }
    return true;
}

static esp_err_t enqueue_print_job(const char *text, uint32_t *job_id, char *error, size_t error_size)
{
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (!printer_connected || !printer_ready_for_transfer) {
        xSemaphoreGive(state_mutex);
        snprintf(error, error_size, "Printer is not connected and ready for USB transfer");
        return ESP_ERR_INVALID_STATE;
    }
    if (uxQueueSpacesAvailable(print_job_queue) == 0) {
        xSemaphoreGive(state_mutex);
        snprintf(error, error_size, "Print queue is full");
        return ESP_ERR_NO_MEM;
    }

    int selected = -1;
    for (unsigned i = 0; i < JOB_RECORD_COUNT; ++i) {
        uint8_t slot = (uint8_t)((next_job_slot + i) % JOB_RECORD_COUNT);
        if (!jobs[slot].used || jobs[slot].status == JOB_USB_TRANSFERRED || jobs[slot].status == JOB_FAILED) {
            selected = slot;
            break;
        }
    }
    if (selected < 0) {
        xSemaphoreGive(state_mutex);
        snprintf(error, error_size, "Job status table is full; try again shortly");
        return ESP_ERR_NO_MEM;
    }

    job_record_t *record = &jobs[selected];
    memset(record, 0, sizeof(*record));
    record->used = true;
    record->id = next_job_id++;
    if (next_job_id == 0) next_job_id = 1;
    record->status = JOB_QUEUED;
    int length = build_tspl_payload(text, record->payload, sizeof(record->payload));
    if (length <= 0 || (size_t)length >= sizeof(record->payload)) {
        record->used = false;
        xSemaphoreGive(state_mutex);
        snprintf(error, error_size, "Could not build label payload");
        return ESP_ERR_INVALID_SIZE;
    }
    record->payload_length = (size_t)length;
    print_item_t queued = {
        .type = PRINT_ITEM_HTTP_JOB,
        .slot = (uint8_t)selected,
        .id = record->id,
    };
    if (xQueueSend(print_job_queue, &queued, 0) != pdTRUE) {
        record->used = false;
        xSemaphoreGive(state_mutex);
        snprintf(error, error_size, "Print queue is full");
        return ESP_ERR_NO_MEM;
    }
    next_job_slot = (uint8_t)((selected + 1) % JOB_RECORD_COUNT);
    *job_id = record->id;
    size_t payload_length = record->payload_length;
    xSemaphoreGive(state_mutex);
    ESP_LOGI(TAG, "Accepted print job %lu; TSPL payload length=%u bytes",
             (unsigned long)*job_id, (unsigned)payload_length);
    return ESP_OK;
}

static bool api_key_matches(httpd_req_t *request)
{
    if (strlen(X_API_KEY) == 0) return false;
    size_t length = httpd_req_get_hdr_value_len(request, "X-API-Key");
    if (length == 0 || length >= 128) return false;
    char supplied[128];
    if (httpd_req_get_hdr_value_str(request, "X-API-Key", supplied, sizeof(supplied)) != ESP_OK) return false;
    size_t expected_length = strlen(X_API_KEY);
    if (length != expected_length) return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < expected_length; ++i) difference |= (unsigned char)(supplied[i] ^ X_API_KEY[i]);
    return difference == 0;
}

static esp_err_t send_json_error(httpd_req_t *request, const char *status, const char *message)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    cJSON_AddStringToObject(json, "error", message);
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == NULL) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    esp_err_t err = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t root_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t read_json_body(httpd_req_t *request, char *body, size_t capacity);

static esp_err_t wifi_setup_page_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, WIFI_SETUP_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_wifi_json(httpd_req_t *request, const char *status, const char *key, const char *value)
{
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    cJSON_AddStringToObject(json, key, value);
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == NULL) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static bool valid_wifi_ssid(const char *ssid)
{
    size_t length = strlen(ssid);
    if (length == 0 || length > 32) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)ssid[i];
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

static bool request_came_via_setup_ap(httpd_req_t *request)
{
    bool setup_mode;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    setup_mode = wifi_setup_mode;
    xSemaphoreGive(state_mutex);
    if (!setup_mode || wifi_ap_netif == NULL) return false;

    esp_netif_ip_info_t ap_ip = {0};
    if (esp_netif_get_ip_info(wifi_ap_netif, &ap_ip) != ESP_OK) return false;
    struct sockaddr_in local_address = {0};
    socklen_t address_length = sizeof(local_address);
    int socket_fd = httpd_req_to_sockfd(request);
    return getsockname(socket_fd, (struct sockaddr *)&local_address, &address_length) == 0 &&
           local_address.sin_family == AF_INET && local_address.sin_addr.s_addr == ap_ip.ip.addr;
}

static esp_err_t wifi_config_handler(httpd_req_t *request)
{
    char body[HTTP_MAX_BODY_BYTES + 1];
    esp_err_t err = read_json_body(request, body, sizeof(body));
    if (err != ESP_OK) return err;
    cJSON *json = cJSON_ParseWithLength(body, (size_t)request->content_len);
    if (json == NULL || !cJSON_IsObject(json)) {
        cJSON_Delete(json);
        return send_json_error(request, "400 Bad Request", "Body must be a JSON object");
    }
    cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    cJSON *password_item = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (!cJSON_IsString(ssid_item) || ssid_item->valuestring == NULL ||
        !cJSON_IsString(password_item) || password_item->valuestring == NULL ||
        !valid_wifi_ssid(ssid_item->valuestring) || strlen(password_item->valuestring) > 64) {
        cJSON_Delete(json);
        return send_json_error(request, "400 Bad Request", "SSID must be 1-32 characters and password 0-64 characters");
    }
    const char *ssid = ssid_item->valuestring;
    const char *password = password_item->valuestring;
    size_t password_length = strlen(password);
    if (password_length != 0 && password_length < 8) {
        cJSON_Delete(json);
        return send_json_error(request, "400 Bad Request", "Wi-Fi passwords must be empty for an open network or at least 8 characters");
    }

    if (!request_came_via_setup_ap(request)) {
        cJSON_Delete(json);
        return send_json_error(request, "403 Forbidden", "Join the printer setup Wi-Fi to change network settings");
    }

    nvs_handle_t nvs = 0;
    err = nvs_open(WIFI_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) err = nvs_set_str(nvs, WIFI_SSID_KEY, ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, WIFI_PASSWORD_KEY, password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    if (err == ESP_OK) nvs_close(nvs);
    else {
        if (nvs != 0) nvs_close(nvs);
        cJSON_Delete(json);
        ESP_LOGE(TAG, "Failed to save Wi-Fi settings (error %s)", esp_err_to_name(err));
        return send_json_error(request, "500 Internal Server Error", "Could not save Wi-Fi settings");
    }

    strlcpy(configured_wifi_ssid, ssid, sizeof(configured_wifi_ssid));
    strlcpy(configured_wifi_password, password, sizeof(configured_wifi_password));
    cJSON_Delete(json);
    ESP_LOGI(TAG, "New Wi-Fi settings saved; starting station connection attempt");
    // Reply before switching networks so the browser can display the result.
    err = send_wifi_json(request, "202 Accepted", "message", "Credentials saved. Connecting to Wi-Fi; the setup page may disconnect when connection succeeds.");
    if (wifi_connect_waiter != NULL) xTaskNotifyGive(wifi_connect_waiter);
    return err;
}

static esp_err_t status_handler(httpd_req_t *request)
{
    bool wifi, connected, ready;
    bool setup_mode;
    esp_netif_ip_info_t sta_ip, ap_ip = {0};
    size_t queue_length = 0;
    bool raw_listening, raw_client, raw_pending;
    uint64_t raw_received, raw_queued, raw_usb, raw_dropped;
    uint32_t raw_completed, raw_failed;
    char raw_error[sizeof(raw_tcp_last_error)];
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    wifi = wifi_connected;
    setup_mode = wifi_setup_mode;
    sta_ip = wifi_ip_info;
    connected = printer_connected;
    ready = printer_ready_for_transfer;
    for (unsigned i = 0; i < JOB_RECORD_COUNT; ++i) {
        if (jobs[i].used && (jobs[i].status == JOB_QUEUED || jobs[i].status == JOB_SENDING)) {
            ++queue_length;
        }
    }
    if (raw_tcp_stream_pending) ++queue_length;
    raw_listening = raw_tcp_listening;
    raw_client = raw_tcp_client_connected;
    raw_pending = raw_tcp_stream_pending;
    raw_received = raw_tcp_received_bytes;
    raw_queued = raw_tcp_queued_bytes;
    raw_usb = raw_tcp_usb_bytes;
    raw_dropped = raw_tcp_dropped_bytes;
    raw_completed = raw_tcp_jobs_completed;
    raw_failed = raw_tcp_jobs_failed;
    memcpy(raw_error, raw_tcp_last_error, sizeof(raw_error));
    xSemaphoreGive(state_mutex);
    if (wifi_ap_netif != NULL) (void)esp_netif_get_ip_info(wifi_ap_netif, &ap_ip);

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    cJSON_AddBoolToObject(json, "wifi_connected", wifi);
    cJSON_AddBoolToObject(json, "wifi_setup_mode", setup_mode);
    char sta_ip_text[16] = "";
    char ap_ip_text[16] = "0.0.0.0";
    if (wifi) snprintf(sta_ip_text, sizeof(sta_ip_text), IPSTR, IP2STR(&sta_ip.ip));
    if (wifi_ap_netif != NULL) snprintf(ap_ip_text, sizeof(ap_ip_text), IPSTR, IP2STR(&ap_ip.ip));
    cJSON_AddStringToObject(json, "wifi_ip", sta_ip_text);
    cJSON_AddStringToObject(json, "wifi_ap_ip", ap_ip_text);
    char ap_ssid[33] = {0};
    wifi_config_t ap_config = {0};
    if (wifi_ap_netif != NULL && esp_wifi_get_config(WIFI_IF_AP, &ap_config) == ESP_OK) {
        memcpy(ap_ssid, ap_config.ap.ssid, sizeof(ap_config.ap.ssid));
        ap_ssid[sizeof(ap_config.ap.ssid)] = '\0';
    }
    cJSON_AddStringToObject(json, "wifi_ap_ssid", ap_ssid);
    cJSON_AddBoolToObject(json, "printer_connected", connected);
    cJSON_AddBoolToObject(json, "printer_ready_for_transfer", ready);
    cJSON_AddStringToObject(json, "printer_vid", "0x1203");
    cJSON_AddStringToObject(json, "printer_pid", "0x0172");
    cJSON_AddStringToObject(json, "printer_model", PRINTER_MODEL);
    cJSON_AddNumberToObject(json, "queue_length", (double)queue_length);
    cJSON_AddBoolToObject(json, "raw_tcp_listening", raw_listening);
    cJSON_AddBoolToObject(json, "raw_tcp_client_connected", raw_client);
    cJSON_AddBoolToObject(json, "raw_tcp_stream_pending", raw_pending);
    cJSON_AddNumberToObject(json, "raw_tcp_port", RAW_TCP_PORT);
    cJSON_AddNumberToObject(json, "raw_tcp_received_bytes", (double)raw_received);
    cJSON_AddNumberToObject(json, "raw_tcp_queued_bytes", (double)raw_queued);
    cJSON_AddNumberToObject(json, "raw_tcp_usb_bytes", (double)raw_usb);
    cJSON_AddNumberToObject(json, "raw_tcp_dropped_bytes", (double)raw_dropped);
    cJSON_AddNumberToObject(json, "raw_tcp_jobs_completed", raw_completed);
    cJSON_AddNumberToObject(json, "raw_tcp_jobs_failed", raw_failed);
    cJSON_AddStringToObject(json, "raw_tcp_last_error", raw_error);
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == NULL) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t read_json_body(httpd_req_t *request, char *body, size_t capacity)
{
    if (request->content_len <= 0) return send_json_error(request, "400 Bad Request", "Request body is required");
    if ((size_t)request->content_len > HTTP_MAX_BODY_BYTES || (size_t)request->content_len >= capacity) {
        return send_json_error(request, "413 Payload Too Large", "Request body exceeds 512 bytes");
    }
    size_t content_type_length = httpd_req_get_hdr_value_len(request, "Content-Type");
    if (content_type_length == 0 || content_type_length >= 64) {
        return send_json_error(request, "415 Unsupported Media Type", "Use Content-Type: application/json");
    }
    char content_type[64];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "application/json", 16) != 0 ||
        (content_type[16] != '\0' && content_type[16] != ';' && content_type[16] != ' ')) {
        return send_json_error(request, "415 Unsupported Media Type", "Use Content-Type: application/json");
    }
    int received = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request, body + received, request->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT) return send_json_error(request, "408 Request Timeout", "Timed out reading request body");
        if (count <= 0) return send_json_error(request, "400 Bad Request", "Incomplete request body");
        received += count;
    }
    body[received] = '\0';
    return ESP_OK;
}

static esp_err_t queue_print_handler(httpd_req_t *request, bool test_job)
{
    if (!api_key_matches(request)) {
        httpd_resp_set_hdr(request, "Connection", "close");
        return send_json_error(request, "401 Unauthorized", "Missing or invalid X-API-Key");
    }
    char body[HTTP_MAX_BODY_BYTES + 1];
    esp_err_t err = read_json_body(request, body, sizeof(body));
    if (err != ESP_OK) return err;
    cJSON *json = cJSON_ParseWithLength(body, (size_t)request->content_len);
    if (json == NULL || !cJSON_IsObject(json)) {
        cJSON_Delete(json);
        return send_json_error(request, "400 Bad Request", "Body must be a JSON object");
    }

    const char *text = "ESP32-S3 USB TEST";
    if (!test_job) {
        cJSON *text_item = cJSON_GetObjectItemCaseSensitive(json, "text");
        if (!cJSON_IsString(text_item) || text_item->valuestring == NULL || !valid_label_text(text_item->valuestring)) {
            cJSON_Delete(json);
            return send_json_error(request, "400 Bad Request", "text must be 1-30 printable basic ASCII characters; quotes and backslashes are not allowed");
        }
        text = text_item->valuestring;
    }
    uint32_t job_id = 0;
    char error[96];
    if (xSemaphoreTake(print_source_mutex, 0) != pdTRUE) {
        cJSON_Delete(json);
        return send_json_error(request, "503 Service Unavailable", "RAW TCP print stream is active; retry after it closes");
    }
    err = enqueue_print_job(text, &job_id, error, sizeof(error));
    xSemaphoreGive(print_source_mutex);
    cJSON_Delete(json);
    if (err == ESP_ERR_INVALID_STATE) return send_json_error(request, "503 Service Unavailable", error);
    if (err == ESP_ERR_NO_MEM) return send_json_error(request, "429 Too Many Requests", error);
    if (err != ESP_OK) return send_json_error(request, "400 Bad Request", error);

    cJSON *response = cJSON_CreateObject();
    if (response == NULL) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    cJSON_AddNumberToObject(response, "job_id", job_id);
    cJSON_AddStringToObject(response, "status", "queued");
    char *response_body = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    if (response_body == NULL) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json");
    err = httpd_resp_send(request, response_body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(response_body);
    return err;
}

static esp_err_t print_handler(httpd_req_t *request)
{
    return queue_print_handler(request, false);
}

static esp_err_t print_test_handler(httpd_req_t *request)
{
    return queue_print_handler(request, true);
}

static esp_err_t job_status_handler(httpd_req_t *request)
{
    const char *last_slash = strrchr(request->uri, '/');
    if (last_slash == NULL || last_slash[1] == '\0') {
        return send_json_error(request, "400 Bad Request", "Provide a numeric job ID");
    }
    char *end = NULL;
    unsigned long id = strtoul(last_slash + 1, &end, 10);
    if (*end != '\0' || id == 0 || id > UINT32_MAX) {
        return send_json_error(request, "400 Bad Request", "Provide a numeric job ID");
    }

    job_status_t status = JOB_UNUSED;
    char error[96] = {0};
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    for (unsigned i = 0; i < JOB_RECORD_COUNT; ++i) {
        if (jobs[i].used && jobs[i].id == (uint32_t)id) {
            status = jobs[i].status;
            memcpy(error, jobs[i].error, sizeof(error));
            break;
        }
    }
    xSemaphoreGive(state_mutex);
    if (status == JOB_UNUSED) return send_json_error(request, "404 Not Found", "Unknown job ID");

    cJSON *json = cJSON_CreateObject();
    if (json == NULL) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    cJSON_AddNumberToObject(json, "job_id", id);
    cJSON_AddStringToObject(json, "status", job_status_name(status));
    if (error[0] != '\0') cJSON_AddStringToObject(json, "error", error);
    if (status == JOB_USB_TRANSFERRED) cJSON_AddStringToObject(json, "note", "USB bytes transferred; physical printing is not confirmed");
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == NULL) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON allocation failed");
    httpd_resp_set_type(request, "application/json");
    esp_err_t err = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static bool api_key_matches(httpd_req_t *request);

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    config.max_open_sockets = 4;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&http_server, &config);
    if (err != ESP_OK) return err;
    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_handler },
        { .uri = "/wifi", .method = HTTP_GET, .handler = wifi_setup_page_handler },
        { .uri = "/wifi/config", .method = HTTP_POST, .handler = wifi_config_handler },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/print", .method = HTTP_POST, .handler = print_handler },
        { .uri = "/api/print-test", .method = HTTP_POST, .handler = print_test_handler },
        { .uri = "/api/jobs/*", .method = HTTP_GET, .handler = job_status_handler },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        err = httpd_register_uri_handler(http_server, &routes[i]);
        if (err != ESP_OK) {
            httpd_stop(http_server);
            http_server = NULL;
            return err;
        }
    }
    ESP_LOGI(TAG, "HTTP server started on port 80; use only on the trusted local network");
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        wifi_connected = false;
        memset(&wifi_ip_info, 0, sizeof(wifi_ip_info));
        xSemaphoreGive(state_mutex);
        ESP_LOGW(TAG, "Wi-Fi disconnected; retrying saved credentials");
        if (wifi_connect_waiter != NULL) xTaskNotify(wifi_connect_waiter, WIFI_MANAGER_EVENT_DISCONNECTED, eSetBits);
        (void)esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        wifi_connected = true;
        wifi_ip_info = event->ip_info;
        xSemaphoreGive(state_mutex);
        ESP_LOGI(TAG, "Wi-Fi connected; IP=" IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Open http://" IPSTR "/", IP2STR(&event->ip_info.ip));
        if (wifi_connect_waiter != NULL) xTaskNotify(wifi_connect_waiter, WIFI_MANAGER_EVENT_CONNECTED, eSetBits);
    }
}

static void load_wifi_credentials(void)
{
    configured_wifi_ssid[0] = '\0';
    configured_wifi_password[0] = '\0';
    nvs_handle_t nvs = 0;
    if (nvs_open(WIFI_CONFIG_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t ssid_size = sizeof(configured_wifi_ssid);
        size_t password_size = sizeof(configured_wifi_password);
        esp_err_t ssid_err = nvs_get_str(nvs, WIFI_SSID_KEY, configured_wifi_ssid, &ssid_size);
        esp_err_t password_err = nvs_get_str(nvs, WIFI_PASSWORD_KEY, configured_wifi_password, &password_size);
        nvs_close(nvs);
        if (ssid_err == ESP_OK && password_err == ESP_OK && valid_wifi_ssid(configured_wifi_ssid) && strlen(configured_wifi_password) <= 64) {
            ESP_LOGI(TAG, "Loaded Wi-Fi settings from local NVS storage");
            return;
        }
    }
    strlcpy(configured_wifi_ssid, WIFI_SSID, sizeof(configured_wifi_ssid));
    strlcpy(configured_wifi_password, WIFI_PASSWORD, sizeof(configured_wifi_password));
    if (valid_wifi_ssid(configured_wifi_ssid) && strlen(configured_wifi_password) <= 64) {
        ESP_LOGI(TAG, "Using initial Wi-Fi settings from ignored project-local credentials header");
        nvs_handle_t write_nvs = 0;
        if (nvs_open(WIFI_CONFIG_NAMESPACE, NVS_READWRITE, &write_nvs) == ESP_OK) {
            if (nvs_set_str(write_nvs, WIFI_SSID_KEY, configured_wifi_ssid) == ESP_OK &&
                nvs_set_str(write_nvs, WIFI_PASSWORD_KEY, configured_wifi_password) == ESP_OK) {
                (void)nvs_commit(write_nvs);
            }
            nvs_close(write_nvs);
        }
    } else {
        configured_wifi_ssid[0] = '\0';
        configured_wifi_password[0] = '\0';
    }
}

static esp_err_t apply_station_credentials(void)
{
    if (!valid_wifi_ssid(configured_wifi_ssid) || strlen(configured_wifi_password) > 64) return ESP_ERR_INVALID_ARG;
    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, configured_wifi_ssid, strlen(configured_wifi_ssid));
    memcpy(wifi_config.sta.password, configured_wifi_password, strlen(configured_wifi_password));
    wifi_config.sta.threshold.authmode = strlen(configured_wifi_password) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

static esp_err_t enable_wifi_setup_ap(void)
{
    uint8_t mac[6] = {0};
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), TAG, "read Wi-Fi MAC failed");
    char ap_ssid[33];
    char ap_password[16];
    snprintf(ap_ssid, sizeof(ap_ssid), WIFI_SETUP_AP_PREFIX "%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(ap_password, sizeof(ap_password), "TSC%02X%02X%02X", mac[3], mac[4], mac[5]);

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, ap_password, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "configure setup AP failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "enable setup AP failed");
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    wifi_setup_mode = true;
    xSemaphoreGive(state_mutex);
    esp_netif_ip_info_t ap_ip = {0};
    if (wifi_ap_netif != NULL) (void)esp_netif_get_ip_info(wifi_ap_netif, &ap_ip);
    ESP_LOGW(TAG, "Wi-Fi setup AP started; SSID=%s password=%s", ap_ssid, ap_password);
    ESP_LOGI(TAG, "Join this AP, then open http://" IPSTR "/wifi", IP2STR(&ap_ip.ip));
    return ESP_OK;
}

static bool wait_for_station_connection(TickType_t timeout_ticks)
{
    TickType_t started = xTaskGetTickCount();
    for (;;) {
        bool connected;
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        connected = wifi_connected;
        xSemaphoreGive(state_mutex);
        if (connected) return true;
        TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout_ticks) return false;
        uint32_t events = 0;
        TickType_t remaining = timeout_ticks - elapsed;
        (void)xTaskNotifyWait(0, UINT32_MAX, &events, remaining);
        if ((events & WIFI_MANAGER_EVENT_NEW_CREDENTIALS) != 0) return false;
    }
}

static void wifi_manager_task(void *arg)
{
    (void)arg;
    wifi_connect_waiter = xTaskGetCurrentTaskHandle();
    if (valid_wifi_ssid(configured_wifi_ssid)) {
        ESP_ERROR_CHECK(apply_station_credentials());
        ESP_LOGI(TAG, "Trying configured Wi-Fi for %u seconds", WIFI_CONNECT_TIMEOUT_MS / 1000);
        if (wait_for_station_connection(pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS))) {
            for (;;) {
                uint32_t events = 0;
                (void)xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
                if ((events & WIFI_MANAGER_EVENT_NEW_CREDENTIALS) != 0) {
                    events &= ~WIFI_MANAGER_EVENT_CONNECTED;
                    ESP_ERROR_CHECK(apply_station_credentials());
                    (void)esp_wifi_disconnect();
                    (void)esp_wifi_connect();
                }
                if ((events & WIFI_MANAGER_EVENT_DISCONNECTED) != 0) {
                    if (!wait_for_station_connection(pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS))) {
                        ESP_ERROR_CHECK(enable_wifi_setup_ap());
                    } else {
                        bool setup_mode;
                        xSemaphoreTake(state_mutex, portMAX_DELAY);
                        setup_mode = wifi_setup_mode;
                        wifi_setup_mode = false;
                        xSemaphoreGive(state_mutex);
                        if (setup_mode) {
                            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
                            ESP_LOGI(TAG, "Station reconnected; setup AP disabled");
                        }
                    }
                }
                if ((events & WIFI_MANAGER_EVENT_CONNECTED) != 0) break;
            }
        } else {
            ESP_ERROR_CHECK(enable_wifi_setup_ap());
        }
    } else {
        ESP_LOGW(TAG, "No configured Wi-Fi network; starting setup AP");
        ESP_ERROR_CHECK(enable_wifi_setup_ap());
    }

    for (;;) {
        uint32_t events = 0;
        (void)xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
        if ((events & WIFI_MANAGER_EVENT_NEW_CREDENTIALS) != 0) {
            events &= ~WIFI_MANAGER_EVENT_CONNECTED;
            ESP_ERROR_CHECK(apply_station_credentials());
            (void)esp_wifi_disconnect();
            (void)esp_wifi_connect();
        }
        if ((events & WIFI_MANAGER_EVENT_CONNECTED) != 0) {
            bool setup_mode;
            xSemaphoreTake(state_mutex, portMAX_DELAY);
            setup_mode = wifi_setup_mode;
            wifi_setup_mode = false;
            xSemaphoreGive(state_mutex);
            if (setup_mode) {
                ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
                ESP_LOGI(TAG, "Station connected; setup AP disabled");
            }
        }
        if ((events & WIFI_MANAGER_EVENT_DISCONNECTED) != 0) {
            if (!wait_for_station_connection(pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS))) {
                bool setup_mode;
                xSemaphoreTake(state_mutex, portMAX_DELAY);
                setup_mode = wifi_setup_mode;
                xSemaphoreGive(state_mutex);
                if (!setup_mode) ESP_ERROR_CHECK(enable_wifi_setup_ap());
            } else {
                bool setup_mode;
                xSemaphoreTake(state_mutex, portMAX_DELAY);
                setup_mode = wifi_setup_mode;
                wifi_setup_mode = false;
                xSemaphoreGive(state_mutex);
                if (setup_mode) {
                    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
                    ESP_LOGI(TAG, "Station reconnected; setup AP disabled");
                }
            }
        }
    }
}

static esp_err_t start_wifi_station(void)
{
    // The ESP-IDF Wi-Fi driver can include the SSID in informational logs.
    // Keep driver/WPA logs suppressed; our application logs only state and IP.
    esp_log_level_set("wifi", ESP_LOG_NONE);
    esp_log_level_set("wifi_init", ESP_LOG_NONE);
    esp_log_level_set("wpa", ESP_LOG_NONE);
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    wifi_sta_netif = esp_netif_create_default_wifi_sta();
    wifi_ap_netif = esp_netif_create_default_wifi_ap();
    if (wifi_sta_netif == NULL || wifi_ap_netif == NULL) return ESP_FAIL;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));
    load_wifi_credentials();
    if (valid_wifi_ssid(configured_wifi_ssid)) ESP_ERROR_CHECK(apply_station_credentials());
    ESP_ERROR_CHECK(esp_wifi_start());
    BaseType_t task = xTaskCreate(wifi_manager_task, "wifi_manager", 4096, NULL, 4, &wifi_connect_waiter);
    if (task != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static bool enqueue_raw_control(print_item_type_t type, uint32_t stream_id)
{
    print_item_t item = { .type = type, .stream_id = stream_id };
    return xQueueSend(print_job_queue, &item, portMAX_DELAY) == pdTRUE;
}

static bool enqueue_raw_bytes(uint32_t stream_id, const uint8_t *data, size_t length, size_t *queued_out)
{
    size_t offset = 0;
    *queued_out = 0;
    while (offset < length) {
        if (raw_stream_failed_snapshot(stream_id)) return false;
        size_t part = length - offset;
        if (part > RAW_CHUNK_SIZE) part = RAW_CHUNK_SIZE;
        print_item_t item = {
            .type = PRINT_ITEM_RAW_DATA,
            .stream_id = stream_id,
            .length = part,
        };
        memcpy(item.data, data + offset, part);
        if (xQueueSend(print_job_queue, &item, RAW_QUEUE_WAIT_TICKS) != pdTRUE) {
            continue; // Bounded-queue backpressure; retain this chunk and retry.
        }
        offset += part;
        *queued_out += part;
        add_raw_counter(&raw_tcp_queued_bytes, part);
        ESP_LOGD(TAG, "RAW TCP stream %lu queued %u USB bytes",
                 (unsigned long)stream_id, (unsigned)part);
    }
    return true;
}

static void close_raw_client(int *client_fd, uint32_t stream_id, size_t session_bytes,
                             bool aborted, const char *reason, bool *source_locked)
{
    if (*client_fd >= 0) {
        shutdown(*client_fd, SHUT_RDWR);
        close(*client_fd);
        *client_fd = -1;
    }
    if (aborted) {
        mark_raw_stream_failed(stream_id, reason != NULL ? reason : "TCP client ended before clean EOF");
    }
    print_item_type_t marker = aborted ? PRINT_ITEM_RAW_ABORT : PRINT_ITEM_RAW_END;
    if (!enqueue_raw_control(marker, stream_id)) {
        mark_raw_stream_failed(stream_id, "Could not queue RAW stream end marker");
    }
    set_raw_client_connected(false);
    ESP_LOGI(TAG, "RAW TCP client closed: stream=%lu received=%u bytes, %s",
             (unsigned long)stream_id, (unsigned)session_bytes, aborted ? "aborted" : "EOF");
    if (*source_locked) {
        xSemaphoreGive(print_source_mutex);
        *source_locked = false;
    }
}

static void raw_tcp_server_task(void *arg)
{
    (void)arg;
    int listen_fd = -1;
    while (listen_fd < 0) {
        listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listen_fd < 0) {
            ESP_LOGE(TAG, "RAW TCP socket creation failed: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in server_address = {0};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(RAW_TCP_PORT);
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);

    while (bind(listen_fd, (struct sockaddr *)&server_address, sizeof(server_address)) != 0 ||
           listen(listen_fd, 2) != 0) {
        ESP_LOGE(TAG, "RAW TCP bind/listen on port %u failed: errno=%d", RAW_TCP_PORT, errno);
        close(listen_fd);
        vTaskDelay(pdMS_TO_TICKS(1000));
        listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (listen_fd < 0) continue;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    }

    set_raw_listener_state(true);
    ESP_LOGI(TAG, "RAW TCP server listening on port %u (unauthenticated, trusted LAN only)", RAW_TCP_PORT);
    int client_fd = -1;
    bool source_locked = false;
    uint32_t stream_id = 0;
    size_t session_bytes = 0;
    TickType_t last_receive_tick = xTaskGetTickCount();
    uint8_t receive_buffer[RAW_CHUNK_SIZE];

    while (true) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(listen_fd, &read_set);
        int max_fd = listen_fd;
        if (client_fd >= 0) {
            FD_SET(client_fd, &read_set);
            if (client_fd > max_fd) max_fd = client_fd;
        }
        struct timeval timeout = { .tv_sec = 0, .tv_usec = 200000 };
        int selected = select(max_fd + 1, &read_set, NULL, NULL, &timeout);
        if (selected < 0) {
            if (errno != EINTR) ESP_LOGW(TAG, "RAW TCP select failed: errno=%d", errno);
            continue;
        }

        if (FD_ISSET(listen_fd, &read_set)) {
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            int accepted = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
            if (accepted >= 0) {
                if (client_fd >= 0) {
                    ESP_LOGW(TAG, "Rejecting extra RAW TCP client while stream %lu is active",
                             (unsigned long)stream_id);
                    shutdown(accepted, SHUT_RDWR);
                    close(accepted);
                } else if (xSemaphoreTake(print_source_mutex, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Rejecting RAW TCP client while another print source is enqueueing");
                    shutdown(accepted, SHUT_RDWR);
                    close(accepted);
                } else if (!printer_ready_snapshot()) {
                    xSemaphoreGive(print_source_mutex);
                    xSemaphoreTake(state_mutex, portMAX_DELAY);
                    raw_tcp_jobs_failed++;
                    snprintf(raw_tcp_last_error, sizeof(raw_tcp_last_error), "Printer unavailable for RAW TCP session");
                    xSemaphoreGive(state_mutex);
                    ESP_LOGW(TAG, "Rejecting RAW TCP client: printer is unavailable");
                    shutdown(accepted, SHUT_RDWR);
                    close(accepted);
                } else {
                    source_locked = true;
                    stream_id = begin_raw_stream_state();
                    session_bytes = 0;
                    if (!enqueue_raw_control(PRINT_ITEM_RAW_BEGIN, stream_id)) {
                        mark_raw_stream_failed(stream_id, "Could not queue RAW stream start marker");
                        close_raw_client(&accepted, stream_id, session_bytes, true,
                                         "Could not start RAW stream", &source_locked);
                    } else {
                        client_fd = accepted;
                        last_receive_tick = xTaskGetTickCount();
                        ESP_LOGI(TAG, "RAW TCP client connected from %s:%u (stream %lu)",
                                 inet_ntoa(peer.sin_addr), ntohs(peer.sin_port), (unsigned long)stream_id);
                    }
                }
            } else if (errno != EINTR) {
                ESP_LOGW(TAG, "RAW TCP accept failed: errno=%d", errno);
            }
        }

        if (client_fd >= 0 && FD_ISSET(client_fd, &read_set)) {
            int received = recv(client_fd, receive_buffer, sizeof(receive_buffer), 0);
            if (received > 0) {
                last_receive_tick = xTaskGetTickCount();
                session_bytes += (size_t)received;
                add_raw_counter(&raw_tcp_received_bytes, (size_t)received);
                ESP_LOGD(TAG, "RAW TCP stream %lu received %d bytes (session total %u)",
                         (unsigned long)stream_id, received, (unsigned)session_bytes);
                size_t queued_now = 0;
                if (!enqueue_raw_bytes(stream_id, receive_buffer, (size_t)received, &queued_now)) {
                    add_raw_counter(&raw_tcp_dropped_bytes, (size_t)received - queued_now);
                    close_raw_client(&client_fd, stream_id, session_bytes, true,
                                     "USB transfer failed while receiving RAW stream", &source_locked);
                }
            } else if (received == 0) {
                bool aborted = raw_stream_failed_snapshot(stream_id);
                close_raw_client(&client_fd, stream_id, session_bytes, aborted,
                                 aborted ? "USB stream failed before TCP EOF" : NULL, &source_locked);
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                char reason[64];
                snprintf(reason, sizeof(reason), "TCP receive failed: errno=%d", errno);
                close_raw_client(&client_fd, stream_id, session_bytes, true, reason, &source_locked);
            }
        }

        if (client_fd >= 0 &&
            (TickType_t)(xTaskGetTickCount() - last_receive_tick) >= pdMS_TO_TICKS(RAW_IDLE_TIMEOUT_MS)) {
            bool failed = raw_stream_failed_snapshot(stream_id);
            ESP_LOGW(TAG, "RAW TCP stream %lu idle timeout", (unsigned long)stream_id);
            close_raw_client(&client_fd, stream_id, session_bytes, failed,
                             failed ? "USB stream failed during idle timeout" : NULL, &source_locked);
        }
        if (client_fd >= 0 && raw_stream_failed_snapshot(stream_id)) {
            close_raw_client(&client_fd, stream_id, session_bytes, true,
                             "USB transfer failed; closing RAW TCP connection", &source_locked);
        }
    }
}

static void mark_job_failed(uint8_t slot, const char *reason)
{
    uint32_t id = jobs[slot].id;
    set_job_state(slot, JOB_FAILED, reason);
    ESP_LOGE(TAG, "Print job %lu failed: %s", (unsigned long)id, reason);
}

static void print_active_configuration_log(usb_device_handle_t device, uint8_t *bulk_out)
{
    if (!print_active_configuration(device, bulk_out)) {
        ESP_LOGE(TAG, "Printer does not expose the expected Bulk OUT endpoint on interface 0 alt 0");
    }
}

static void inspect_device(uint8_t address)
{
    if (open_device != NULL) {
        if (device_gone) {
            pending_new_device = true;
            pending_device_address = address;
        } else {
            ESP_LOGW(TAG, "Ignoring USB address %u while address %u is active", address, open_device_address);
        }
        return;
    }
    usb_device_handle_t device = NULL;
    esp_err_t err = usb_host_device_open(client_handle, address, &device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not open USB address %u: %s", address, esp_err_to_name(err));
        return;
    }
    const usb_device_desc_t *descriptor = NULL;
    err = usb_host_get_device_descriptor(device, &descriptor);
    if (err != ESP_OK || descriptor == NULL) {
        ESP_LOGE(TAG, "Could not read USB device descriptor: %s", esp_err_to_name(err));
        usb_host_device_close(client_handle, device);
        return;
    }
    ESP_LOGI(TAG, "USB device connected: address=%u VID:PID=%04x:%04x",
             address, descriptor->idVendor, descriptor->idProduct);
    if (descriptor->idVendor != 0x1203 || descriptor->idProduct != 0x0172) {
        ESP_LOGI(TAG, "USB device is not the configured TSC printer; closing it");
        usb_host_device_close(client_handle, device);
        return;
    }
    open_device = device;
    open_device_address = address;
    device_gone = false;
    ESP_LOGI(TAG, "TSC TTP-244 Pro connected (1203:0172)");
    set_printer_state(true, false);
    print_active_configuration_log(device, &bulk_out_endpoint);
    if (bulk_out_endpoint == 0) return;

    err = usb_host_interface_claim(client_handle, open_device,
                                   PRINTER_INTERFACE_NUMBER, PRINTER_ALT_SETTING);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not claim printer interface %u alt %u: %s",
                 PRINTER_INTERFACE_NUMBER, PRINTER_ALT_SETTING, esp_err_to_name(err));
        return;
    }
    interface_claimed = true;
    set_printer_state(true, true);
    ESP_LOGI(TAG, "Printer interface claimed; ready for USB transfer on endpoint 0x%02x", bulk_out_endpoint);
}

static void cleanup_active_device(void)
{
    if (print_transfer != NULL) {
        if (!print_transfer_complete) return;
        ESP_ERROR_CHECK(usb_host_transfer_free(print_transfer));
        print_transfer = NULL;
    }
    if (interface_claimed && open_device != NULL) {
        esp_err_t err = usb_host_interface_release(client_handle, open_device, PRINTER_INTERFACE_NUMBER);
        if (err != ESP_OK) ESP_LOGW(TAG, "Interface release during cleanup: %s", esp_err_to_name(err));
        interface_claimed = false;
    }
    if (open_device != NULL) {
        esp_err_t err = usb_host_device_close(client_handle, open_device);
        if (err != ESP_OK) ESP_LOGW(TAG, "Device close during cleanup: %s", esp_err_to_name(err));
    }
    if (usb_raw_stream_active) {
        usb_raw_stream_failed = true;
        mark_raw_stream_failed(usb_raw_stream_id, "Printer disconnected during RAW TCP stream");
    }
    open_device = NULL;
    open_device_address = 0;
    bulk_out_endpoint = 0;
    device_gone = false;
    set_printer_state(false, false);
}

static void handle_device_gone(usb_device_handle_t device)
{
    if (open_device == NULL || open_device != device) return;
    ESP_LOGI(TAG, "TSC printer disconnected");
    device_gone = true;
    set_printer_state(false, false);
    if (print_transfer != NULL) return; // finish_print_transfer owns in-flight completion and cleanup.
    if (current_job_active) {
        mark_job_failed(current_job_slot, "Printer disconnected before USB transfer completed");
        current_job_active = false;
    }
    cleanup_active_device();
}

static void submit_raw_pending_chunk(void);

static void finish_print_transfer(void)
{
    if (print_transfer == NULL || !print_transfer_complete) return;
    bool complete = print_transfer_status == USB_TRANSFER_STATUS_COMPLETED &&
                    print_transfer_actual_bytes == print_transfer->num_bytes;
    ESP_LOGI(TAG, "Print transfer result: %s, actual bytes=%d/%d",
             transfer_status_name(print_transfer_status), print_transfer_actual_bytes,
             print_transfer->num_bytes);
    if (active_transfer_type == TRANSFER_RAW_DATA) {
        size_t actual = print_transfer_actual_bytes > 0 ? (size_t)print_transfer_actual_bytes : 0;
        size_t submitted = (size_t)print_transfer_submitted_bytes;
        if (actual > submitted) actual = submitted;
        add_raw_counter(&raw_tcp_usb_bytes, actual);
        usb_raw_stream_transferred_bytes += actual;
        raw_pending_offset += actual;
        if (print_transfer_status == USB_TRANSFER_STATUS_COMPLETED && actual > 0 &&
            raw_pending_offset < raw_pending_length && !device_gone) {
            ESP_LOGD(TAG, "RAW USB transfer was short; resubmitting remaining %u bytes",
                     (unsigned)(raw_pending_length - raw_pending_offset));
            ESP_ERROR_CHECK(usb_host_transfer_free(print_transfer));
            print_transfer = NULL;
            print_transfer_complete = false;
            active_transfer_type = TRANSFER_NONE;
            submit_raw_pending_chunk();
            return;
        }
        bool full_chunk = print_transfer_status == USB_TRANSFER_STATUS_COMPLETED &&
                          raw_pending_offset == raw_pending_length;
        if (full_chunk) {
            raw_pending_length = 0;
            raw_pending_offset = 0;
        } else {
            size_t unsent = raw_pending_length - raw_pending_offset;
            add_raw_counter(&raw_tcp_dropped_bytes, unsent);
            usb_raw_stream_failed = true;
            mark_raw_stream_failed(usb_raw_stream_id, transfer_status_name(print_transfer_status));
            raw_pending_length = 0;
            raw_pending_offset = 0;
        }
    } else if (current_job_active) {
        if (complete) {
            set_job_state(current_job_slot, JOB_USB_TRANSFERRED, NULL);
            ESP_LOGI(TAG, "Print job %lu completed USB transmission; physical printing is not confirmed",
                     (unsigned long)jobs[current_job_slot].id);
        } else {
            mark_job_failed(current_job_slot, transfer_status_name(print_transfer_status));
        }
        current_job_active = false;
    }
    ESP_ERROR_CHECK(usb_host_transfer_free(print_transfer));
    print_transfer = NULL;
    print_transfer_complete = false;
    active_transfer_type = TRANSFER_NONE;
    if (device_gone) cleanup_active_device();
}

static void submit_queued_print_job(const print_item_t *queued)
{
    if (queued->slot >= JOB_RECORD_COUNT || !jobs[queued->slot].used || jobs[queued->slot].id != queued->id) return;
    current_job_slot = queued->slot;
    current_job_active = true;
    set_job_state(current_job_slot, JOB_SENDING, NULL);

    if (!open_device || !interface_claimed || !printer_ready_snapshot()) {
        mark_job_failed(current_job_slot, "Printer disconnected before job was sent");
        current_job_active = false;
        return;
    }
    job_record_t *job = &jobs[current_job_slot];
    ESP_LOGI(TAG, "Sending job %lu (%u TSPL bytes) to Bulk OUT endpoint 0x%02x",
             (unsigned long)job->id, (unsigned)job->payload_length, bulk_out_endpoint);
    esp_err_t err = usb_host_transfer_alloc(job->payload_length, 0, &print_transfer);
    if (err != ESP_OK) {
        print_transfer = NULL;
        mark_job_failed(current_job_slot, "USB transfer allocation failed");
        current_job_active = false;
        return;
    }
    memcpy(print_transfer->data_buffer, job->payload, job->payload_length);
    print_transfer->num_bytes = (int)job->payload_length;
    print_transfer->device_handle = open_device;
    print_transfer->bEndpointAddress = bulk_out_endpoint;
    print_transfer->callback = print_transfer_callback;
    print_transfer->context = NULL;
    print_transfer_complete = false;
    print_transfer_actual_bytes = 0;
    print_transfer_submitted_bytes = (int)job->payload_length;
    active_transfer_type = TRANSFER_HTTP_JOB;

    err = usb_host_transfer_submit(print_transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bulk OUT submit for job %lu failed: %s", (unsigned long)job->id, esp_err_to_name(err));
        ESP_ERROR_CHECK(usb_host_transfer_free(print_transfer));
        print_transfer = NULL;
        active_transfer_type = TRANSFER_NONE;
        mark_job_failed(current_job_slot, esp_err_to_name(err));
        current_job_active = false;
        return;
    }
    ESP_LOGI(TAG, "Job %lu submitted (%u bytes); USB completion is not proof of physical printing",
             (unsigned long)job->id, (unsigned)job->payload_length);
}

static void submit_raw_pending_chunk(void)
{
    if (raw_pending_offset >= raw_pending_length) return;
    size_t remaining = raw_pending_length - raw_pending_offset;
    if (!open_device || !interface_claimed || !printer_ready_snapshot() || usb_raw_stream_failed) {
        add_raw_counter(&raw_tcp_dropped_bytes, remaining);
        raw_pending_length = 0;
        raw_pending_offset = 0;
        usb_raw_stream_failed = true;
        mark_raw_stream_failed(usb_raw_stream_id, "Printer unavailable while sending RAW stream");
        return;
    }

    esp_err_t err = usb_host_transfer_alloc(remaining, 0, &print_transfer);
    if (err != ESP_OK) {
        add_raw_counter(&raw_tcp_dropped_bytes, remaining);
        raw_pending_length = 0;
        raw_pending_offset = 0;
        usb_raw_stream_failed = true;
        mark_raw_stream_failed(usb_raw_stream_id, "USB transfer allocation failed for RAW data");
        return;
    }
    memcpy(print_transfer->data_buffer, raw_pending_data + raw_pending_offset, remaining);
    print_transfer->num_bytes = (int)remaining;
    print_transfer_submitted_bytes = (int)remaining;
    print_transfer->device_handle = open_device;
    print_transfer->bEndpointAddress = bulk_out_endpoint;
    print_transfer->callback = print_transfer_callback;
    print_transfer->context = NULL;
    print_transfer_complete = false;
    print_transfer_actual_bytes = 0;
    active_transfer_type = TRANSFER_RAW_DATA;
    err = usb_host_transfer_submit(print_transfer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RAW USB transfer submission failed: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(usb_host_transfer_free(print_transfer));
        print_transfer = NULL;
        active_transfer_type = TRANSFER_NONE;
        add_raw_counter(&raw_tcp_dropped_bytes, remaining);
        raw_pending_length = 0;
        raw_pending_offset = 0;
        usb_raw_stream_failed = true;
        mark_raw_stream_failed(usb_raw_stream_id, esp_err_to_name(err));
        return;
    }
    ESP_LOGD(TAG, "RAW USB transfer submitted: stream=%lu bytes=%u endpoint=0x%02x",
             (unsigned long)usb_raw_stream_id, (unsigned)remaining, bulk_out_endpoint);
}

static void finish_raw_stream(uint32_t stream_id, bool aborted)
{
    if (!usb_raw_stream_active || usb_raw_stream_id != stream_id) {
        mark_raw_stream_failed(stream_id, "Unexpected RAW stream end marker");
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        raw_tcp_jobs_failed++;
        xSemaphoreGive(state_mutex);
        return;
    }
    bool failed = aborted || usb_raw_stream_failed || !printer_ready_snapshot();
    if (failed) {
        if (!usb_raw_stream_failed) {
            mark_raw_stream_failed(stream_id, aborted ? "TCP client closed before clean EOF" : "Printer unavailable at stream end");
        }
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        raw_tcp_jobs_failed++;
        xSemaphoreGive(state_mutex);
        ESP_LOGE(TAG, "RAW stream %lu failed: queued=%llu USB=%llu bytes",
                 (unsigned long)stream_id,
                 (unsigned long long)usb_raw_stream_queued_bytes,
                 (unsigned long long)usb_raw_stream_transferred_bytes);
    } else {
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        raw_tcp_jobs_completed++;
        xSemaphoreGive(state_mutex);
        ESP_LOGI(TAG, "RAW stream %lu USB transfer complete: queued=%llu USB=%llu bytes; physical printing is not confirmed",
                 (unsigned long)stream_id,
                 (unsigned long long)usb_raw_stream_queued_bytes,
                 (unsigned long long)usb_raw_stream_transferred_bytes);
    }
    usb_raw_stream_active = false;
    usb_raw_stream_failed = false;
    usb_raw_stream_id = 0;
    raw_pending_length = 0;
    raw_pending_offset = 0;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    if (raw_tcp_stream_id == stream_id) raw_tcp_stream_pending = false;
    xSemaphoreGive(state_mutex);
}

static void process_raw_print_item(const print_item_t *item)
{
    switch (item->type) {
    case PRINT_ITEM_RAW_BEGIN:
        if (usb_raw_stream_active) {
            mark_raw_stream_failed(usb_raw_stream_id, "Overlapping RAW stream start");
            usb_raw_stream_failed = true;
        }
        usb_raw_stream_active = true;
        usb_raw_stream_id = item->stream_id;
        usb_raw_stream_failed = !open_device || !interface_claimed || !printer_ready_snapshot();
        usb_raw_stream_queued_bytes = 0;
        usb_raw_stream_transferred_bytes = 0;
        raw_pending_length = 0;
        raw_pending_offset = 0;
        if (usb_raw_stream_failed) {
            mark_raw_stream_failed(item->stream_id, "Printer unavailable when RAW stream reached USB queue");
        } else {
            ESP_LOGI(TAG, "USB owner began RAW stream %lu", (unsigned long)item->stream_id);
        }
        break;
    case PRINT_ITEM_RAW_DATA:
        if (!usb_raw_stream_active || usb_raw_stream_id != item->stream_id ||
            usb_raw_stream_failed || item->length == 0 || item->length > RAW_CHUNK_SIZE) {
            add_raw_counter(&raw_tcp_dropped_bytes, item->length);
            usb_raw_stream_failed = true;
            mark_raw_stream_failed(item->stream_id, "RAW data discarded because USB stream is unavailable");
            break;
        }
        usb_raw_stream_queued_bytes += item->length;
        memcpy(raw_pending_data, item->data, item->length);
        raw_pending_length = item->length;
        raw_pending_offset = 0;
        submit_raw_pending_chunk();
        break;
    case PRINT_ITEM_RAW_END:
        finish_raw_stream(item->stream_id, false);
        break;
    case PRINT_ITEM_RAW_ABORT:
        finish_raw_stream(item->stream_id, true);
        break;
    default:
        break;
    }
}

static void usb_host_library_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t event_flags = 0;
        esp_err_t err = usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "USB Host library event handling failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

static void usb_host_service_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Installing USB host controller on GPIO 19/20");
    const usb_host_config_t host_config = { .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    BaseType_t library_task = xTaskCreate(usb_host_library_task, "usb_host_lib", 4096, NULL, 5, NULL);
    ESP_ERROR_CHECK(library_task == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    device_event_queue = xQueueCreate(USB_EVENT_QUEUE_LENGTH, sizeof(device_event_t));
    ESP_ERROR_CHECK(device_event_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = USB_EVENT_QUEUE_LENGTH,
        .async = { .client_event_callback = client_event_callback, .callback_arg = NULL },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &client_handle));
    ESP_LOGI(TAG, "USB Host client ready; waiting for TSC printer 1203:0172");

    while (true) {
        esp_err_t err = usb_host_client_handle_events(client_handle, USB_JOB_POLL_TICKS);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "USB client event handling failed: %s", esp_err_to_name(err));
        }
        device_event_t event;
        while (xQueueReceive(device_event_queue, &event, 0) == pdTRUE) {
            if (event.event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
                inspect_device(event.address);
            } else if (event.event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
                handle_device_gone(event.handle);
            }
        }
        finish_print_transfer();

        if (open_device == NULL && pending_new_device) {
            uint8_t address = pending_device_address;
            pending_new_device = false;
            inspect_device(address);
        }
        if (print_transfer == NULL && !current_job_active) {
            print_item_t item;
            if (xQueueReceive(print_job_queue, &item, 0) == pdTRUE) {
                if (item.type == PRINT_ITEM_HTTP_JOB) {
                    submit_queued_print_job(&item);
                } else {
                    process_raw_print_item(&item);
                }
            }
        }
    }
}

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    state_mutex = xSemaphoreCreateMutex();
    print_source_mutex = xSemaphoreCreateMutex();
    print_job_queue = xQueueCreate(PRINT_QUEUE_LENGTH, sizeof(print_item_t));
    ESP_ERROR_CHECK(state_mutex == NULL || print_source_mutex == NULL || print_job_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(start_http_server());
    ESP_ERROR_CHECK(start_wifi_station());

    BaseType_t usb_task = xTaskCreate(usb_host_service_task, "usb_host", 6144, NULL, 6, NULL);
    ESP_ERROR_CHECK(usb_task == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t raw_tcp_task = xTaskCreate(raw_tcp_server_task, "raw_tcp_9100", 6144, NULL, 5, NULL);
    ESP_ERROR_CHECK(raw_tcp_task == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
