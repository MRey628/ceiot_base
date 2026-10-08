#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include <bmp280.h>

#include "esp_http_client.h"

#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>

#ifndef APP_CPU_NUM
#define APP_CPU_NUM PRO_CPU_NUM
#endif


#define WIFI_SSID       ""
#define WIFI_PASSWORD   ""

#define SERVER_URL      ""

#define DEVICE_ID "01"

#define WIFI_CONNECTED_BIT BIT0

#define I2C_MASTER_SDA 5
#define I2C_MASTER_SCL 6

static const char *TAG = "HTTP_TEST";
static EventGroupHandle_t wifi_event_group;

static char flagErrorLectura;

static time_t now;
struct tm ti;
static char hora[24];

/* -----------------------------------------------------------
 * WiFi
 * --------------------------------------------------------- */
//Funciones error
char GetErrorLectura(void)
{
    return flagErrorLectura;
}

void SetErrorLectura(char estado)
{
    flagErrorLectura=estado;
}

//Funciones timestamp
static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Hora sincronizada por SNTP");
}

static void sntp_start(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");   // respaldo
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}

static bool wait_for_time_sync(int max_retries)
{
    int retry = 0;
    while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET &&
           ++retry <= max_retries) {
        ESP_LOGI(TAG, "Esperando hora... (%d/%d)", retry, max_retries);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return retry <= max_retries;
}

void bmp280_test(bmp280_t *dev) 
{
    bmp280_params_t params;
    bmp280_init_default_params(&params);
    memset(dev, 0, sizeof(bmp280_t));

    ESP_ERROR_CHECK(bmp280_init_desc(dev, BMP280_I2C_ADDRESS_0, 0, I2C_MASTER_SDA, I2C_MASTER_SCL));
    ESP_ERROR_CHECK(bmp280_init(dev, &params));

    bool bme280p = dev->id == BME280_CHIP_ID;
    printf("BMP280: found %s\n", bme280p ? "BME280" : "BMP280");
}

void bmp280_read(bmp280_t *dev, float *pressure, float *temperature, float *humidity) {

    if (bmp280_read_float(dev, temperature, pressure, humidity) != ESP_OK) {
        printf("Temperature/pressure reading failed\n");
        SetErrorLectura(1);
        return;
    }
    SetErrorLectura(0);
    //printf("Pressure: %.2f Pa, Temperature: %.2f C", *pressure, *temperature);
    printf("Time: %s, Pressure: %.2f Pa, Temperature: %.2f C", hora, *pressure, *temperature);


    bool bme280p = dev->id == BME280_CHIP_ID;
    if (bme280p)
        printf(", Humidity: %.2f\n", *humidity);
    else
        printf("\n");
}


/* -----------------------------------------------------------
 * WiFi
 * --------------------------------------------------------- */

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {

        ESP_LOGI(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "Got IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));

        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg)
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL
        )
    );

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );

    ESP_LOGI(TAG, "Connecting to WiFi...");
}

/* -----------------------------------------------------------
 * HTTP event
 * --------------------------------------------------------- */

static esp_err_t http_event_handler(
    esp_http_client_event_t *evt)
{
    switch (evt->event_id) {

    case HTTP_EVENT_ON_DATA:

        printf("%.*s",
               evt->data_len,
               (char *)evt->data);

        break;

    default:
        break;
    }

    return ESP_OK;
}

/* -----------------------------------------------------------
 * GET
 * --------------------------------------------------------- */

static void http_get(void)
{
    ESP_LOGI(TAG, "GET %s", SERVER_URL);

    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .event_handler = http_event_handler,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    esp_http_client_set_method(
        client,
        HTTP_METHOD_GET
    );

    esp_err_t err =
        esp_http_client_perform(client);

    if (err == ESP_OK) {

        ESP_LOGI(TAG,
                 "GET status = %d, content_length = %lld",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));

    } else {

        ESP_LOGE(TAG,
                 "GET failed: %s",
                 esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

/* -----------------------------------------------------------
 * POST
 * --------------------------------------------------------- */

static char *POST_DATA_TEMPLATE = "id="DEVICE_ID"&ts=%s&t=%0.2f&p=%0.2f&e=%s";
//static char *POST_DATA_TEMPLATE = "id="DEVICE_ID"&t=%0.2f&p=%0.2f&e=%s";
//static char *POST_DATA_TEMPLATE = "id="DEVICE_ID"&t=%0.2f&h=%0.2f";


static void http_post(float *pressure, float *temperature, float *humidity) {

    //char post_data[64];
    //char post_data[64+3]; // 64 + 3 para el error de lectura
    char post_data[64+3+24]; // 64 + 3 para el error de lectura + 24 para el timestamp

    static char errorLectura[3];
    now = time(NULL);

    localtime_r(&now, &ti);
    strftime(hora, sizeof(hora), "%Y-%m-%d %H:%M:%S", &ti);
    ESP_LOGI(TAG, "Medicion a las %s (ts=%lld)", hora, (long long)now);

    if(GetErrorLectura()==1)
    {
        errorLectura[0]='E';
        errorLectura[1]='R';
    }
    else
    {
        errorLectura[0]='O';
        errorLectura[1]='K';
    }
    errorLectura[2]='\0';

    sprintf(post_data,POST_DATA_TEMPLATE, hora, *temperature, *pressure,errorLectura);
    //sprintf(post_data,POST_DATA_TEMPLATE, *temperature, *pressure,errorLectura);
    //sprintf(post_data,POST_DATA_TEMPLATE, *temperature, *humidity);


    ESP_LOGI(TAG, "POST %s", SERVER_URL);
    ESP_LOGI(TAG, "POST_DATA %s", post_data);

    esp_http_client_config_t config = {
        .url = SERVER_URL,
        .event_handler = http_event_handler,
    };

    esp_http_client_handle_t client =
        esp_http_client_init(&config);

    esp_http_client_set_method(
        client,
        HTTP_METHOD_POST
    );

    esp_http_client_set_header(
        client,
        "Content-Type",
        "application/x-www-form-urlencoded"
    );

    esp_http_client_set_post_field(
        client,
        post_data,
        strlen(post_data)
    );

    esp_err_t err =
        esp_http_client_perform(client);

    if (err == ESP_OK) {

        ESP_LOGI(TAG,
                 "POST status = %d",
                 esp_http_client_get_status_code(client));

    } else {

        ESP_LOGE(TAG,
                 "POST failed: %s",
                 esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

/* -----------------------------------------------------------
 * Main
 * --------------------------------------------------------- */

void app_main(void)
{
    ESP_ERROR_CHECK(
        nvs_flash_init()
    );

    bmp280_t dev;

    ESP_ERROR_CHECK(i2cdev_init());

    ESP_LOGI(TAG, "bmp280 init...");
    bmp280_test(&dev);    

    wifi_init();

    /* Wait for WiFi */
    xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY
    );

    ESP_LOGI(TAG, "WiFi connected");

    //Codigo inicio timestamp
    setenv("TZ", "<-03>3", 1);   // Argentina, UTC-3 sin horario de verano
    tzset();
    sntp_start();

    vTaskDelay(pdMS_TO_TICKS(1000));

    wait_for_time_sync(10);

    float pressure, temperature, humidity;
    while (1) { 
       bmp280_read(&dev, &pressure, &temperature, &humidity);
       http_post(&pressure, &temperature, &humidity);
       vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
