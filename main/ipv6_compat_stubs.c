#include "sdkconfig.h"

#if !CONFIG_LWIP_IPV6

#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "lwip/inet.h"

int esp_netif_get_all_ip6(esp_netif_t *esp_netif, esp_ip6_addr_t if_ip6[])
{
    (void)esp_netif;
    (void)if_ip6;
    return 0;
}

esp_ip6_addr_type_t esp_netif_ip6_get_addr_type(esp_ip6_addr_t *ip6_addr)
{
    (void)ip6_addr;
    return ESP_IP6_ADDR_IS_UNKNOWN;
}

const struct in6_addr in6addr_any = IN6ADDR_ANY_INIT;

#endif
