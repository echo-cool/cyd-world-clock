#include "apNat.h"

#include <WiFi.h>

esp_netif_t *apNetif()
{
    return esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
}

void apOfferUpstreamDns()
{
    esp_netif_t *ap = apNetif();
    if (!ap) return;

    IPAddress up = WiFi.dnsIP();
    if ((uint32_t)up == 0) up = IPAddress(8, 8, 8, 8); // fallback if none learned

    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = (uint32_t)up;

    esp_netif_dhcps_stop(ap);
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    uint8_t offerDns = 0x02; // OFFER_DNS: include the DNS option in DHCP leases
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offerDns, sizeof(offerDns));
    esp_netif_dhcps_start(ap);
}

esp_err_t apNaptEnable()
{
    esp_netif_t *ap = apNetif();
    return ap ? esp_netif_napt_enable(ap) : ESP_ERR_INVALID_STATE;
}

void apNaptDisable()
{
    esp_netif_t *ap = apNetif();
    if (ap) esp_netif_napt_disable(ap);
}
