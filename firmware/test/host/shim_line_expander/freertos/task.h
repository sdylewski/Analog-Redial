#define ulTaskNotifyTake(a, b) (0)
#define xTaskNotifyGive(t) ((void)(t))
#define xTaskCreate(f, n, s, a, p, h) (pdPASS)
