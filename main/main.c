#include <stdio.h>

#include "esp_system.h"
#include "esp_random.h"
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
esp_err_t common_handler(httpd_req_t* req);
esp_err_t sensor_handler(httpd_req_t* req);
esp_err_t output_handler(httpd_req_t* req);
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
        //registrando URI para enviar informacion de los sensores (entradas)
        httpd_uri_t data_uri = {
            .uri = "/sensor",
            .method = HTTP_GET,
            .handler = sensor_handler,
            .user_ctx = ctx
        };
        httpd_register_uri_handler(server_local, &data_uri);

        //registrando URI para obtener informacion de los relevadores (salidas)
        httpd_uri_t output_uri = {
            .uri = "/output",
            .method = HTTP_POST,
            .handler = output_handler,
            .user_ctx = ctx
        };
        httpd_register_uri_handler(server_local, &output_uri);
        
        //registrando URI comun para servir archivos
        httpd_uri_t common_uri = {
            .uri = "/*",
            .method = HTTP_GET,
            .handler = common_handler,
            .user_ctx = ctx
        };
        httpd_register_uri_handler(server_local, &common_uri);

        return server_local;
    }
    else{
        ESP_LOGE(TAG, "Fallo en el arranque del servidor web en el puerto: %i ", config.server_port);
        free(ctx); //can i free this variable always??? outside the else
    }
    
    return NULL;
}

/*Su proposito es regresar los archivos que le pida el navegador
 * 1. Determinar que archivo se esta solicitando 
 * 2. Leer el archivo de la memoria flash
 * 3. Enviarlo por el request de http, si es muy grande hacerlo por partes
 */
esp_err_t common_handler(httpd_req_t* req){

    return ESP_OK;
}

//Colectar informacion disponible de cada sensor y responder dichos datos al navegador/ cliente web
esp_err_t sensor_handler(httpd_req_t* req){

    uint32_t random_data = 0;
    size_t hdr_len = httpd_req_get_url_query_len(req) + 1;

    if ( hdr_len != 0 ){
        char* sensor_type_buff = (char*) malloc(hdr_len * sizeof(char) );
        httpd_req_get_url_query_str(req, sensor_type_buff, hdr_len);

        char sensor_type[15];
        sscanf(sensor_type_buff, "type=%s", sensor_type);

        if( strcmp(sensor_type, "temp") == 0 ){
            random_data = esp_random()%45;
        }

        if( strcmp(sensor_type, "humi") == 0 ){
            random_data = esp_random()%100;   
        }
        ESP_LOGI(TAG, "El tipo de sensor es %s y el dato es %lu", sensor_type, random_data);
        free(sensor_type_buff);
    }
    else
        ESP_LOGE(TAG, "No hay query string params por leer");
        //Can I send a 500 status error code and return ESP_FAIL

    char data_response[20];
    sprintf(data_response, "%lu", random_data);
    ESP_LOGI(TAG, "La respuesta de temperatura es %s", data_response);

    //Envia la respuesta por http
    httpd_resp_sendstr(req, data_response);

    return ESP_OK;
}

//Guardar el estado del output que mando el navegador/cliente web, y actualizar el componente fisico
esp_err_t output_handler(httpd_req_t* req){
    //can i use this code here??
    int total_len = (int)req->content_len;
    int current_len = 0;
    char* buffer = ((rest_server_context_t*)(req->user_ctx))->scratch;
    int received = 0;

    if( total_len >= SCRATCH_BUFSIZE){
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Content too long");
        return ESP_FAIL;
    }

    while( current_len < total_len ){
        received = httpd_req_recv(req, buffer + current_len, total_len);
        if( received <= 0 ){
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Content too long");
            return ESP_FAIL;
        }
        current_len+=received;
    }

    buffer[total_len] = '\0';
    return ESP_OK;
}