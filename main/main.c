#include <stdio.h>
#include "driver/gpio.h"
#include "aht20.h"
#include "driver/i2c.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"
#include "cJSON.h"
#include "esp_spiffs.h"
#include <fcntl.h>
#include "esp_vfs.h"
#include "mbedtls/md5.h"
#include "esp_system.h"
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
#define USERS_SPACE             "storage"
#define KEY_USER_NAME           "default_user" 
#define KEY_PASS_NAME           "default_pass" 
#define DNS_LOCAL_NAME          "esp-home"        
#define I2C_SCL_PIN             GPIO_NUM_22   
#define I2C_SDA_PIN             GPIO_NUM_21   
#define I2C_MASTER_NUM          I2C_NUM_0       
#define I2C_MASTER_FREQ_HZ      100000     //Frecuencia de reloj I2C en Hertz              

#define CHECK_FILE_EXTENSION(filename, ext) (strcasecmp(&filename[strlen(filename) - strlen(ext)], ext) == 0)

const char* TAG = "SMART HOME";
void connect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
void disconnect_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
void compute_md5(const unsigned char* input, size_t ilen, unsigned char output[16]);
static httpd_handle_t start_webserver();
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filepath);
esp_err_t common_handler(httpd_req_t* req);
esp_err_t sensor_handler(httpd_req_t* req);
esp_err_t output_handler(httpd_req_t* req);
esp_err_t auth_handler(httpd_req_t* req);
esp_err_t init_fs(const char* mount_path);
void init_mdns(void);
void i2c_bus_init(void);

static aht20_dev_handle_t aht20 = NULL;
typedef struct rest_server_context {
    char base_path[MAX_FS_PATH_SIZE + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;//esta estructura permite relacionar el archivo que pide el servidor web con un buffer para enviarlo

void app_main(void){

    esp_err_t ret = nvs_flash_init();
    if( ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND){
        nvs_flash_erase();
        ret =nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    //Guarda en la flash el usuario y contrasenia default para accesar a los recursos del servidor web
    nvs_handle_t handle_nvs;
    nvs_open(USERS_SPACE, NVS_READWRITE, &handle_nvs);
    
    //Si la primera vez regresa un size de 0 quiere decir que no se ha guardado el dato
    size_t required_size = 0;
    nvs_get_str(handle_nvs, KEY_USER_NAME, NULL, required_size);
    if ( required_size == 0 ){
        const char* user_name = CONFIG_USER_NAME;
        nvs_set_str(handle_nvs, KEY_USER_NAME, user_name);
    }
    
    nvs_get_str(handle_nvs, KEY_PASS_NAME, NULL, required_size);
    if ( required_size == 0 ){
        const char* pass_name = CONFIG_USER_PASSWORD;
        unsigned char pass_name_hash[16];
        compute_md5((unsigned char*)pass_name, strlen(pass_name), pass_name_hash);
        char hashed_pass[17];
        memset(hashed_pass, 0 , 17);
        char tmp[4];

        for(int index = 0; index < 16; index++){
            memset(tmp, 0 , 4);
            sprintf(tmp, "%02x", pass_name_hash[index]);
            strcat(hashed_pass, tmp);
        }

        nvs_set_str(handle_nvs, KEY_PASS_NAME, hashed_pass);
    }
    //Asegurandonos de que los cambios queden guardados en la flash
    nvs_commit(handle_nvs);
    nvs_close(handle_nvs);

    init_mdns();
    netbiosns_init();
    netbiosns_set_name(DNS_LOCAL_NAME);

    esp_netif_init();//inicializa la libreria de red que maneja todos los recursos de la libreria TCP/IP
    esp_event_loop_create_default(); //crea un loop de default que es el que se encarga de arrojar mensajes cuando adquirimos una ip el estado de wifi cambia

    example_connect(); //dispara la conexion para configurar el ssid y el password usa el menuconfig

    /*Variable donde se guardan las configuraciones de mi servidor*/
    static httpd_handle_t server = NULL;
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server);

    server = start_webserver();
    
    char* base_path = FS_BASE_PATH;
    init_fs(base_path);

    gpio_set_direction(GPIO_NUM_16, GPIO_MODE_OUTPUT);
    
    aht20_i2c_config_t i2c_conf = {
        .i2c_port = I2C_MASTER_NUM,
        .i2c_addr = AHT20_ADDRRES_0,
    };
    i2c_bus_init();
    aht20_new_sensor(&i2c_conf, &aht20);
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
        
        //registrando URI para autenticar usuarios
        httpd_uri_t auth_uri = {
            .uri = "/authentication",
            .method = HTTP_POST,
            .handler = auth_handler,
            .user_ctx = ctx
        };
        httpd_register_uri_handler(server_local, &auth_uri);
        
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

    float response_data = 0.0;
    uint32_t temperature_raw;
    uint32_t humidity_raw;
    float temperature;
    float humidity;
    size_t hdr_len = httpd_req_get_url_query_len(req) + 1;

    if ( hdr_len != 0 ){
        char* sensor_type_buff = (char*) malloc(hdr_len * sizeof(char) );
        httpd_req_get_url_query_str(req, sensor_type_buff, hdr_len);

        char sensor_type[15];
        sscanf(sensor_type_buff, "type=%s", sensor_type);

        if( strcmp(sensor_type, "temp") == 0 ){
            aht20_read_temperature_humidity(aht20, &temperature_raw, &temperature, &humidity_raw, &humidity);
            response_data = temperature;
        }

        if( strcmp(sensor_type, "humi") == 0 ){
            aht20_read_temperature_humidity(aht20, &temperature_raw, &temperature, &humidity_raw, &humidity);
            response_data = humidity;   
        }
        ESP_LOGI(TAG, "El tipo de sensor es %s y el dato es %2.2f", sensor_type, response_data);
        free(sensor_type_buff);
    }
    else{
        ESP_LOGE(TAG, "No hay query string params por leer");
        httpd_resp_send_500(req); //send a 500 status error code and return ESP_FAIL
        return ESP_FAIL;
    }
        
    char data_response[20];
    sprintf(data_response, "%2.2f", response_data);
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
    gpio_set_level(GPIO_NUM_16, output_value);
    ESP_LOGI(TAG, "La respuesta del request POST es %s and integer value is %d", buffer, output_value);

    //Envia la respuesta por http
    const char res[] = "OK";
    httpd_resp_sendstr(req, res);

    return ESP_OK;
}

/*
 * Nos permitira validar que el usuario este validado para acceder a los recursos del servidor (sensores/actuadores) 
 * y pagina web
 */
esp_err_t auth_handler(httpd_req_t* req){
    char* buff = ((rest_server_context_t*)(req->user_ctx))->scratch; 
    int received = 0;
    received = httpd_req_recv(req, buff, req->content_len);

    if( received <= 0){
        ESP_LOGE(TAG, "El contenido del request no contiene datos");
        return ESP_FAIL;
    }
    buff[req->content_len] = '\0'; //necesario para usarlo en la funcion JSON parse

    const cJSON* root = cJSON_Parse(buff);
    const cJSON* user = NULL;
    const cJSON* hashed_pass = NULL;
    
    if( root == NULL){
        ESP_LOGE(TAG, "The root JSON could not be parsed");
        return ESP_FAIL;
    }

    user = cJSON_GetObjectItem(root, "user");
    hashed_pass = cJSON_GetObjectItem(root, "pass");

    cJSON* http_response = cJSON_CreateObject();

    if( ( user->valuestring != NULL) && ( hashed_pass->valuestring != NULL)){
        /*Abriendo la particion de memoria para leer el usuario y el hash de la contraseña para comparar con el request http*/
        nvs_handle_t handle_nvs;
        nvs_open(USERS_SPACE, NVS_READONLY, &handle_nvs);
        
        size_t required_size = 0;
        nvs_get_str(handle_nvs, KEY_USER_NAME, NULL, &required_size);
        char* user_buff = (char*) malloc(sizeof(char)*required_size);
        nvs_get_str(handle_nvs, KEY_USER_NAME, user_buff, &required_size);
        
        required_size = 0;
        nvs_get_str(handle_nvs, KEY_PASS_NAME, NULL, &required_size);
        char* pass_buff = (char*) malloc(sizeof(char)*required_size);
        nvs_get_str(handle_nvs, KEY_PASS_NAME, pass_buff, &required_size);
        nvs_close(handle_nvs);

        if( (strcmp(user_buff, user->valuestring) == 0) && (strcmp(pass_buff, hashed_pass->valuestring) == 0)){
            ESP_LOGI(TAG, " El usuario ha sido autenticado correctamente");
            const char* http_ok = "ESP_OK";
            cJSON_AddStringToObject(http_response, "response", http_ok);
        }
        else{
            ESP_LOGE(TAG, "El usuario no se pudo autenticar, usuario/password incorrecto");
            const char* http_fail = "ESP_FAIL";
            cJSON_AddStringToObject(http_response, "response", http_fail);
        }
     
        char* http_response_str = cJSON_Print(http_response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, http_response_str);
        
        free(user_buff);
        free(pass_buff);
        http_response_str = NULL;
    } 
    else{
        ESP_LOGE(TAG, "El usuario no se pudo autenticar");
        httpd_resp_send_500(req);
    }
    
    cJSON_Delete(http_response);
    cJSON_Delete(root);
    return ESP_OK;
}

/* Inicializa el filesystem dentro de la memoria flash y montalo
 * en caso de no tener paginas libres, se borra el contenido de esa region de memoria y se monta
 */
esp_err_t init_fs(const char* mount_path){

    esp_vfs_spiffs_conf_t conf = {
        .base_path = mount_path,
        .partition_label = NULL,
        .max_files = 3, //cantidad maxima de archivos que podemos abrir a la vez
        .format_if_mount_failed = true
    };

    esp_err_t result = esp_vfs_spiffs_register(&conf);

    if( result != ESP_OK){
        if( result == ESP_FAIL )
            ESP_LOGE(TAG, "Fallo el montaje del filesystem en el path %s", mount_path);
        else
            ESP_LOGE(TAG, "Fallo el montaje del filesystem con error (%s)", esp_err_to_name(result));
    }

    return result;
}

/*
 * Esta API nos va a permitir calcular el hash de la contraseña que 
 * guardaremos en la flash con MD5.
 */
void compute_md5(const unsigned char* input, size_t ilen, unsigned char output[16]){
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, input, ilen);
    mbedtls_md5_finish(&ctx, output);
    mbedtls_md5_free(&ctx);
}

/* Esta API nos va a permitir inicializar el servicio de Multicast DNS
 * La instancia de nuestro hostname sera indicada en hostname_set()
 */
void init_mdns(void){
    mdns_init();
    mdns_hostname_set(DNS_LOCAL_NAME);

    mdns_txt_item_t serviceData[] = {
        {"board", "esp32"},
        {"path","/"}
    };

    mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, serviceData, sizeof(serviceData)/sizeof(serviceData[0]));
}

void i2c_bus_init(void){
    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = I2C_SCL_PIN,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ
    };
    esp_err_t ret = i2c_param_config(I2C_MASTER_NUM, &i2c_conf);
    ESP_ERROR_CHECK(ret);

    ret = i2c_driver_install(I2C_MASTER_NUM, i2c_conf.mode, 0, 0, 0);
    ESP_ERROR_CHECK(ret);
}