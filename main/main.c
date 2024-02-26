#include <stdio.h>

#include <fcntl.h>
#include "esp_vfs.h"
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
#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

const char* TAG = "SMART HOME";
void connect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
void disconnect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static httpd_handle_t start_webserver();
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filepath);
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

/* Configura el tipo de archivo a retornar dependiendo de la extension */
esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filepath){
    const char *type = "text/plain";
    if (CHECK_FILE_EXTENSION(filepath, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
        type = "application/javascript";
    } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
        type = "text/xml";
    }
    return httpd_resp_set_type(req, type);
}

/*Su proposito es regresar los archivos que le pida el navegador
 * 1. Determinar que archivo se esta solicitando 
 * 2. Leer el archivo de la memoria flash
 * 3. Enviarlo por el request de http, si es muy grande hacerlo por partes
 */
esp_err_t common_handler(httpd_req_t* req){

    char filepath[FILE_PATH_MAX];

    rest_server_context_t* ctx = (rest_server_context_t*)req->user_ctx;
    strlcpy( filepath, ctx->base_path, sizeof(filepath) );
    ESP_LOGI(TAG, "El path del archivo solicitado es %s", filepath);

    //Usando la uri y el filepath, determinar la ruta del archivo dentro del filesystem
    if( req->uri[strlen(req->uri) - 1] == '/' )
        strlcat(filepath, "/index.html", sizeof(filepath));
    else
        strlcat(filepath, req->uri, sizeof(filepath));

    int fd = open(filepath, O_RDONLY, 0);
    if( fd == -1){
        ESP_LOGE(TAG,"No se pudo encontrar/abrir el archivo %s", filepath);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    set_content_type_from_file(req, filepath);

    char* chunk = ctx->scratch;
    ssize_t read_bytes;

    do{
        read_bytes = read(fd, chunk, SCRATCH_BUFSIZE);
        if( read_bytes == -1)
            ESP_LOGE(TAG, "No se pudo leer el archivo %s", filepath);
        else if( read_bytes > 0 ){
            if ( httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK){
                ESP_LOGE(TAG, "No se pudo leer el archivo %s al cliente", filepath);
                close(fd);
                httpd_resp_sendstr_chunk(req, NULL);
                httpd_resp_send_500(req);
                return ESP_FAIL;
            }
        }
    }while( read_bytes > 0 );


    close(fd);
    httpd_resp_send_chunk(req, NULL, 0);//puede esta linea ser sendstr???
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
    int total_len = (int)req->content_len;
    int current_len = 0;
    char* buffer = ((rest_server_context_t*)(req->user_ctx))->scratch;
    int received = 0;

    if( total_len >= SCRATCH_BUFSIZE){
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Content too long");
        return ESP_FAIL;
    }

    //Como los datos que recibimos son mucho menores que el buffer scracth de 10Kbytes, podemos evitar este codigo y leer solo una vez sin el ciclo
    while( current_len < total_len ){
        received = httpd_req_recv(req, buffer + current_len, total_len);
        if( received <= 0 ){
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Content too long");
            return ESP_FAIL;
        }
        current_len+=received;
    }
    buffer[total_len] = '\0';

    int output_value = atoi(buffer);
    ESP_LOGI(TAG, "La respuesta del request POST es %s and integer value is %d", buffer, output_value);

    //Envia la respuesta por http
    const char res[20] = "OK";
    httpd_resp_sendstr(req, res);

    return ESP_OK;
}