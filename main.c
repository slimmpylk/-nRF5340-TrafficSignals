#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <stdlib.h>
#include <string.h>

// LED-pinien määritykset
static const struct gpio_dt_spec red_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);    // Punainen
static const struct gpio_dt_spec green_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);  // Vihreä

// UART-initialisointi
#define UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)
static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);

#define STACK_SIZE 1024
#define PRIORITY 5
#define MAX_MSG_LEN 256  // Suurensin pituutta varmuuden vuoksi

// Määrittele säikeiden pinot ja ohjauslohkot
K_THREAD_STACK_DEFINE(uart_receive_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(dispatcher_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(red_task_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(yellow_task_stack, STACK_SIZE);
K_THREAD_STACK_DEFINE(green_task_stack, STACK_SIZE);

struct k_thread uart_receive_thread_data;
struct k_thread dispatcher_thread_data;
struct k_thread red_task_thread_data;
struct k_thread yellow_task_thread_data;
struct k_thread green_task_thread_data;

// Määrittele FIFO-puskurit
K_FIFO_DEFINE(red_fifo);
K_FIFO_DEFINE(yellow_fifo);
K_FIFO_DEFINE(green_fifo);

// Määrittele condition variablet ja muteksit
struct k_condvar red_condvar;
struct k_condvar yellow_condvar;
struct k_condvar green_condvar;
struct k_mutex led_mutex;    // Suojaa FIFO-puskureita ja condition variableja
struct k_mutex light_mutex;  // Varmistaa, että vain yksi valo on päällä kerrallaan

// Määrittele komentojono
K_FIFO_DEFINE(command_queue);

struct command_item {
    void *fifo_reserved; // Ensimmäinen kenttä varattu kernelille
    char color;
};

// Määrittele ring buffer
struct ring_buffer {
    char buffer[MAX_MSG_LEN];
    int head;
    int tail;
    int count;
};

struct ring_buffer uart_buffer = {.head = 0, .tail = 0, .count = 0};
char uart_msg[MAX_MSG_LEN];
int uart_msg_index = 0;

// Määrittele led_item-rakenne
struct led_item {
    void *fifo_reserved; // Ensimmäinen kenttä varattu kernelille
    int duration;
};

// UART-initialisointifunktio
int init_uart(void) {
    if (!device_is_ready(uart_dev)) {
        printk("UART device not ready!\n");
        return 1;
    }
    printk("UART initialized successfully\n");
    return 0;
}

// GPIO-initialisointifunktio
int init_gpio(void) {
    if (!device_is_ready(red_led.port) || !device_is_ready(green_led.port)) {
        printk("One or more LED devices not ready!\n");
        return -1;
    }

    gpio_pin_configure_dt(&red_led, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&green_led, GPIO_OUTPUT_INACTIVE);

    printk("GPIOs initialized successfully\n");
    return 0;
}

// Ring buffer -lisäysfunktio
void ring_buffer_put(struct ring_buffer *rb, const char *msg) {
    for (int i = 0; msg[i] != '\0'; i++) {
        rb->buffer[rb->head] = msg[i];
        rb->head = (rb->head + 1) % MAX_MSG_LEN;
        rb->count++;
        // Vältetään ylivuoto
        if (rb->count > MAX_MSG_LEN) {
            rb->count = MAX_MSG_LEN;
            rb->tail = (rb->tail + 1) % MAX_MSG_LEN;
        }
    }
}

// Ring buffer -luku funktio
int ring_buffer_get(struct ring_buffer *rb, char *msg) {
    if (rb->count == 0) {
        return -1;  // Puskuri on tyhjä
    }

    int i = 0;
    while (rb->count > 0 && i < MAX_MSG_LEN - 1) {
        msg[i] = rb->buffer[rb->tail];
        rb->tail = (rb->tail + 1) % MAX_MSG_LEN;
        rb->count--;
        i++;
    }
    msg[i] = '\0'; // Lopetetaan merkkijono
    return 0;
}

// UART-vastaanottotehtävä
void uart_receive_task(void *p1, void *p2, void *p3) {
    char rc;
    printk("UART Receive Task Started\n");

    while (true) {
        // Tarkistetaan UART-tulo
        if (uart_poll_in(uart_dev, &rc) == 0) {
            // Lähetetään merkki takaisin terminaaliin
            uart_poll_out(uart_dev, rc);

            // Tallennetaan merkki viestipuskuriin
            uart_msg[uart_msg_index++] = rc;

            // Tarkistetaan viestin loppu (rivinvaihto '\n' tai '\r')
            if (rc == '\n' || rc == '\r' || uart_msg_index >= MAX_MSG_LEN) {
                uart_msg[uart_msg_index - 1] = '\0';  // Lopetetaan merkkijono

                printk("Received message: %s\n", uart_msg);

                // Laitetaan koko viesti ring bufferiin
                ring_buffer_put(&uart_buffer, uart_msg);

                // Nollataan viesti-indeksi seuraavaa viestiä varten
                uart_msg_index = 0;
            }
        }
        k_msleep(10);  // Pieni viive, jotta vältetään kiireinen odotus
    }
}

// Dispatcher-tehtävä
void dispatcher_task(void *p1, void *p2, void *p3) {
    printk("Dispatcher Task Started\n");

    char msg[MAX_MSG_LEN];
    char *ptr;
    char color;
    int duration;
    char command_sequence[MAX_MSG_LEN] = {0};
    int repeat_times = 1;

    while (true) {
        // Haetaan seuraava viesti ring bufferista
        if (ring_buffer_get(&uart_buffer, msg) == 0) {
            printk("Dispatcher received message: %s\n", msg);

            // Etsitään 'T' viestistä
            char *t_ptr = strchr(msg, 'T');
            if (t_ptr != NULL) {
                // Kopioidaan sekvenssi ennen 'T'-merkkiä
                int sequence_len = t_ptr - msg;
                strncpy(command_sequence, msg, sequence_len);
                command_sequence[sequence_len] = '\0';

                // Luetaan toistojen määrä 'T' jälkeen
                if (sscanf(t_ptr, "T,%d", &repeat_times) != 1) {
                    printk("Invalid repeat format in message\n");
                    repeat_times = 1; // Oletusarvo 1, jos jäsennys epäonnistuu
                }
            } else {
                // Ei 'T'-merkkiä, käytetään koko viestiä sekvenssinä
                strcpy(command_sequence, msg);
                repeat_times = 1;
            }

            // Toistetaan sekvenssi 'repeat_times' kertaa
            for (int r = 0; r < repeat_times; r++) {
                ptr = command_sequence;
                while (*ptr != '\0') {
                    int n = 0;
                    if (sscanf(ptr, "%c,%d%n", &color, &duration, &n) != 2) {
                        printk("Invalid format in sequence\n");
                        break;
                    }

                    printk("Color: %c, Duration: %d ms\n", color, duration);

                    struct led_item *led_data = k_malloc(sizeof(struct led_item));
                    if (led_data != NULL) {
                        led_data->duration = duration;

                        k_mutex_lock(&led_mutex, K_FOREVER);
                        switch (color) {
                            case 'R':
                                k_fifo_put(&red_fifo, led_data);
                                k_condvar_signal(&red_condvar);
                                break;
                            case 'G':
                                k_fifo_put(&green_fifo, led_data);
                                k_condvar_signal(&green_condvar);
                                break;
                            case 'Y':
                                k_fifo_put(&yellow_fifo, led_data);
                                k_condvar_signal(&yellow_condvar);
                                break;
                            default:
                                printk("Unknown color received: %c\n", color);
                                k_free(led_data);
                                break;
                        }
                        k_mutex_unlock(&led_mutex);

                        // Lisää komento command_queue-jonoon
                        struct command_item *cmd_item = k_malloc(sizeof(struct command_item));
                        if (cmd_item != NULL) {
                            cmd_item->color = color;
                            k_fifo_put(&command_queue, cmd_item);
                        } else {
                            printk("Memory allocation failed for command_item\n");
                        }
                    } else {
                        printk("Memory allocation failed for led_item\n");
                    }

                    ptr += n;  // Siirretään osoitinta eteenpäin jäsennetyn osuuden verran

                    // Ohitetaan mahdolliset ylimääräiset pilkut tai välilyönnit
                    while (*ptr == ',' || *ptr == ' ') {
                        ptr++;
                    }
                }
            }
        }

        k_msleep(10);  // Pieni viive, jotta vältetään kiireinen odotus
    }
}

// Punaisen valon tehtävä
void red_light_task(void *p1, void *p2, void *p3) {
    while (true) {
        // Odota, että FIFOssa on dataa
        k_mutex_lock(&led_mutex, K_FOREVER);
        while (k_fifo_is_empty(&red_fifo)) {
            k_condvar_wait(&red_condvar, &led_mutex, K_FOREVER);
        }
        k_mutex_unlock(&led_mutex);

        // Odota vuoroasi command_queue-jonossa
        struct command_item *cmd_item = NULL;
        while (true) {
            cmd_item = k_fifo_peek_head(&command_queue);
            if (cmd_item != NULL && cmd_item->color == 'R') {
                // On meidän vuoro
                k_fifo_get(&command_queue, K_NO_WAIT);
                k_free(cmd_item);
                break;
            } else {
                // Ei meidän vuoro, odotetaan hetki
                k_msleep(1);
            }
        }

        // Prosessoi oma FIFO
        struct led_item *item;
        k_mutex_lock(&led_mutex, K_FOREVER);
        item = k_fifo_get(&red_fifo, K_NO_WAIT);
        k_mutex_unlock(&led_mutex);

        if (item != NULL) {
            k_mutex_lock(&light_mutex, K_FOREVER);
            printk("Red light ON for %d ms\n", item->duration);
            gpio_pin_set_dt(&green_led, 0);  // Varmista, että vihreä LED on pois päältä
            gpio_pin_set_dt(&red_led, 1);    // Sytytä punainen LED
            k_msleep(item->duration);
            gpio_pin_set_dt(&red_led, 0);    // Sammuta punainen LED
            printk("Red light OFF\n");
            k_mutex_unlock(&light_mutex);
            k_free(item);
        }
    }
}

// Vihreän valon tehtävä
void green_light_task(void *p1, void *p2, void *p3) {
    while (true) {
        // Odota, että FIFOssa on dataa
        k_mutex_lock(&led_mutex, K_FOREVER);
        while (k_fifo_is_empty(&green_fifo)) {
            k_condvar_wait(&green_condvar, &led_mutex, K_FOREVER);
        }
        k_mutex_unlock(&led_mutex);

        // Odota vuoroasi command_queue-jonossa
        struct command_item *cmd_item = NULL;
        while (true) {
            cmd_item = k_fifo_peek_head(&command_queue);
            if (cmd_item != NULL && cmd_item->color == 'G') {
                // On meidän vuoro
                k_fifo_get(&command_queue, K_NO_WAIT);
                k_free(cmd_item);
                break;
            } else {
                // Ei meidän vuoro, odotetaan hetki
                k_msleep(1);
            }
        }

        // Prosessoi oma FIFO
        struct led_item *item;
        k_mutex_lock(&led_mutex, K_FOREVER);
        item = k_fifo_get(&green_fifo, K_NO_WAIT);
        k_mutex_unlock(&led_mutex);

        if (item != NULL) {
            k_mutex_lock(&light_mutex, K_FOREVER);
            printk("Green light ON for %d ms\n", item->duration);
            gpio_pin_set_dt(&red_led, 0);    // Varmista, että punainen LED on pois päältä
            gpio_pin_set_dt(&green_led, 1);  // Sytytä vihreä LED
            k_msleep(item->duration);
            gpio_pin_set_dt(&green_led, 0);  // Sammuta vihreä LED
            printk("Green light OFF\n");
            k_mutex_unlock(&light_mutex);
            k_free(item);
        }
    }
}

// Keltaisen valon tehtävä
void yellow_light_task(void *p1, void *p2, void *p3) {
    while (true) {
        // Odota, että FIFOssa on dataa
        k_mutex_lock(&led_mutex, K_FOREVER);
        while (k_fifo_is_empty(&yellow_fifo)) {
            k_condvar_wait(&yellow_condvar, &led_mutex, K_FOREVER);
        }
        k_mutex_unlock(&led_mutex);

        // Odota vuoroasi command_queue-jonossa
        struct command_item *cmd_item = NULL;
        while (true) {
            cmd_item = k_fifo_peek_head(&command_queue);
            if (cmd_item != NULL && cmd_item->color == 'Y') {
                // On meidän vuoro
                k_fifo_get(&command_queue, K_NO_WAIT);
                k_free(cmd_item);
                break;
            } else {
                // Ei meidän vuoro, odotetaan hetki
                k_msleep(1);
            }
        }

        // Prosessoi oma FIFO
        struct led_item *item;
        k_mutex_lock(&led_mutex, K_FOREVER);
        item = k_fifo_get(&yellow_fifo, K_NO_WAIT);
        k_mutex_unlock(&led_mutex);

        if (item != NULL) {
            k_mutex_lock(&light_mutex, K_FOREVER);
            printk("Yellow light (Red + Green) ON for %d ms\n", item->duration);
            gpio_pin_set_dt(&red_led, 1);    // Sytytä punainen LED
            gpio_pin_set_dt(&green_led, 1);  // Sytytä vihreä LED
            k_msleep(item->duration);
            gpio_pin_set_dt(&red_led, 0);    // Sammuta punainen LED
            gpio_pin_set_dt(&green_led, 0);  // Sammuta vihreä LED
            printk("Yellow light OFF\n");
            k_mutex_unlock(&light_mutex);
            k_free(item);
        }
    }
}

int main(void) {
    int ret = init_uart();
    if (ret != 0) {
        printk("UART initialization failed!\n");
        return ret;
    }

    ret = init_gpio();
    if (ret != 0) {
        printk("GPIO initialization failed!\n");
        return ret;
    }

    // Odota, että kaikki alustuu
    k_msleep(100);

    printk("Started serial read example\n");

    // Alusta condition variablet ja muteksit
    k_condvar_init(&red_condvar);
    k_condvar_init(&yellow_condvar);
    k_condvar_init(&green_condvar);
    k_mutex_init(&led_mutex);
    k_mutex_init(&light_mutex); // Uusi muteksi valojen hallintaan

    // Luo säikeet tehtäville
    k_thread_create(&uart_receive_thread_data, uart_receive_stack, STACK_SIZE,
                    uart_receive_task, NULL, NULL, NULL,
                    PRIORITY, 0, K_NO_WAIT);

    k_thread_create(&dispatcher_thread_data, dispatcher_stack, STACK_SIZE,
                    dispatcher_task, NULL, NULL, NULL,
                    PRIORITY, 0, K_NO_WAIT);

    k_thread_create(&red_task_thread_data, red_task_stack, STACK_SIZE,
                    red_light_task, NULL, NULL, NULL,
                    PRIORITY, 0, K_NO_WAIT);

    k_thread_create(&green_task_thread_data, green_task_stack, STACK_SIZE,
                    green_light_task, NULL, NULL, NULL,
                    PRIORITY, 0, K_NO_WAIT);

    k_thread_create(&yellow_task_thread_data, yellow_task_stack, STACK_SIZE,
                    yellow_light_task, NULL, NULL, NULL,
                    PRIORITY, 0, K_NO_WAIT);

    return 0;
}
