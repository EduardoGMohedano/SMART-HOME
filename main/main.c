#include <stdio.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#define MAX_FS_PATH_SIZE        15
#define SCRATCH_BUFSIZE         10240
#define FILE_PATH_MAX           40
#define FS_BASE_PATH            "/www"

const char* TAG = "SMART HOME";
void connect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
void disconnect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static httpd_handle_t start_webserver();
typedef struct rest_server_context {
    char base_path[MAX_FS_PATH_SIZE + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;//essta estructura permite relacionar el archivo que pide el servidor web con un buffer para enviarlo

void app_main(void){

    esp_err_t ret = nvs_flash_init();
    if( ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND){
        nvs_flash_erase();
        ret =nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);


    esp_netif_init();//inicializa la libreria de red que maneja todos los recursos de la libreria TCP/IP
    esp_event_loop_create_default(); //crea un loop de default que es el que se encarga de arrojar mensajes cuando adquirimos una ip el estado de wifi cambia

    example_connect(); //dispara la conexion para configurar el ssid y el password usa el menuconfig

    /*Variable donde se guardan las configuraciones de mi servidor*/
    static httpd_handle_t server = NULL;
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server);

    server = start_webserver();
}


void connect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data){
    httpd_handle_t* server = (httpd_handle_t*)arg;
    if( *server == NULL ){
        ESP_LOGI(TAG, "Arrancando web server");
        *server = start_webserver();
    }
}

void disconnect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data){
    ESP_LOGE(TAG, "La conexion Wifi se ha perdido");
    httpd_handle_t* server = (httpd_handle_t*)arg;
    if( *server ){
        if (  httpd_stop(*server) == ESP_OK ){
            ESP_LOGI(TAG, "Deteniendo web server");
            server = NULL;
        }
       else 
        ESP_LOGE(TAG, "Fallo el detenido del web server");
    }
}

static httpd_handle_t start_webserver(){
    rest_server_context_t* ctx = calloc(1, sizeof(rest_server_context_t));
    strcpy(ctx->base_path, FS_BASE_PATH);

    httpd_handle_t server_local = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;// how to explain this shittt??
    
    if( httpd_start( &server_local, &config ) == ESP_OK ){
        ESP_LOGI(TAG, "Arrancando servidor web en el puerto: %i ", config.server_port);

    }
    
    free(ctx);
    return NULL;

}