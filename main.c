
#include "dhcp.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("M. Sami GURPINAR <sami.gurpinar@gmail.com>");
MODULE_DESCRIPTION("A lkm for detection and prevention of Arp Poisoning");
MODULE_VERSION("0.1"); 

#define eth_is_bcast(addr) (((addr)[0] & 0xffff) && ((addr)[2] & 0xffff) && ((addr)[4] & 0xffff))

static struct nf_hook_ops *arpho = NULL;
static struct nf_hook_ops *ipho = NULL;

static int arp_is_valid(
    struct sk_buff *skb, u16 ar_op, unsigned char *sha, u32 sip, unsigned char *tha, u32 tip);

static unsigned int arp_hook(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
    struct arphdr *arp;
    unsigned char *arp_ptr;
    unsigned char *sha,*tha;
    u32 sip,tip;
    struct net_device *dev;
    const struct in_device *indev;
    const struct in_ifaddr *ifa;
    const struct neighbour *hw;
    const struct dhcp_snooping_entry *entry;
    unsigned int status = NF_ACCEPT;
      
    if (unlikely(!skb))
        return NF_DROP;

    dev = skb->dev;
    indev = in_dev_get(dev);
    
    arp = arp_hdr(skb);
    arp_ptr = (unsigned char *)(arp + 1);
    sha	= arp_ptr;
    arp_ptr += dev->addr_len;
    memcpy(&sip, arp_ptr, 4);
    arp_ptr += 4;
    tha	= arp_ptr;
    arp_ptr += dev->addr_len;
    memcpy(&tip, arp_ptr, 4);

    if (arp_is_valid(skb, ntohs(arp->ar_op), sha, sip, tha, tip)) {
        for (ifa = indev->ifa_list; 
            ifa; 
            ifa = ifa->ifa_next) {
                if (ifa->ifa_address == tip) {
                    hw = neigh_lookup(&arp_tbl, &sip, dev);   
                    if (hw && memcmp(hw->ha, sha, dev->addr_len) != 0) {
                        status = NF_DROP;
                    }
                
                    entry = find_dhcp_snooping_entry(sip);
                    if (entry && memcmp(entry->mac, sha, ETH_ALEN) != 0) {
                        printk(KERN_INFO "kdai:  ARP spoofing detected on %s from %pM\n", ifa->ifa_label, sha);
                        status = NF_DROP;
                    } else status = NF_ACCEPT;             
        
                    break;
                } else status = NF_DROP; 
        }
   
    } else status = NF_DROP;
    
    return status;
}


static unsigned int ip_hook(void *priv, struct sk_buff *skb, const struct nf_hook_state *state) {
    struct udphdr *udp;
    struct dhcp *payload;
    unsigned char *opt;
    u8 dhcp_packet_type;
    u32 lease_time;
    struct timespec ts;
    struct dhcp_snooping_entry *entry;
    unsigned int status = NF_ACCEPT;

    if (unlikely(!skb))
        return NF_DROP;

    udp = udp_hdr(skb);
    
    if (udp->source == htons(DHCP_SERVER_PORT) || udp->source == htons(DHCP_CLIENT_PORT)) {
        payload = (struct dhcp *) ((unsigned char *)udp + sizeof(struct udphdr));
        
        if (dhcp_is_valid(skb)) {
            memcpy(&dhcp_packet_type, &payload->bp_options[2], 1);
            
            switch (dhcp_packet_type) {
                case DHCP_ACK:{
                    for (opt = payload->bp_options; 
                        *opt != DHCP_OPTION_END; 
                        opt += opt[1] + 2) {
                            if (*opt == DHCP_OPTION_LEASE_TIME) {
                                memcpy(&lease_time, &opt[2], 4);
                                break;
                            }
                    }
                    printk(KERN_INFO "kdai:  DHCPACK of %pI4\n", &payload->yiaddr);
                    getnstimeofday(&ts);
                    entry = find_dhcp_snooping_entry(payload->yiaddr);
                    if (entry) {
                        memcpy(entry->mac, payload->chaddr, ETH_ALEN);
                        entry->lease_time = ntohl(lease_time);
                        entry->expires = ts.tv_sec + ntohl(lease_time);
                    } else {
                        insert_dhcp_snooping_entry(
                            payload->chaddr, payload->yiaddr, ntohl(lease_time), ts.tv_sec + ntohl(lease_time));
                    }
                    break;
                }
                case DHCP_RELEASE:{
                    printk(KERN_INFO "kdai:  DHCPRELEASE of %pI4\n", &payload->ciaddr);
                    delete_dhcp_snooping_entry(payload->ciaddr);
                    break;
                }
            default:
                break;
            }
      
        } else status = NF_DROP;
    }
   
    return status;
}


static int arp_is_valid(struct sk_buff *skb, u_int16_t ar_op, 
                        unsigned char *sha, u32 sip, unsigned char *tha, u32 tip)  {
    int status = 1;
    struct ethhdr *eth;
    unsigned char shaddr[ETH_ALEN],dhaddr[ETH_ALEN];

    eth = eth_hdr(skb);
    memcpy(shaddr, eth->h_source, ETH_ALEN);
    memcpy(dhaddr, eth->h_dest, ETH_ALEN);

    switch (ar_op) {
        case ARPOP_REQUEST:{
            if ((memcmp(sha, shaddr, ETH_ALEN) != 0) || !eth_is_bcast(dhaddr) || 
                ipv4_is_multicast(sip) || ipv4_is_loopback(sip) || ipv4_is_zeronet(sip)) {
                    printk(KERN_INFO "kdai:  Invalid ARP request from %pM\n", sha);
                    status = 0;
            }
            break;
        }
        case ARPOP_REPLY:{
            if ((memcmp(tha, dhaddr, ETH_ALEN) != 0) || (memcmp(sha, shaddr, ETH_ALEN) != 0) || 
                ipv4_is_multicast(tip) || ipv4_is_loopback(tip) || ipv4_is_zeronet(tip) || 
                ipv4_is_multicast(sip) || ipv4_is_loopback(sip) || ipv4_is_zeronet(sip)) {
                    printk(KERN_INFO "kdai:  Invalid ARP reply from %pM\n", sha);
                    status = 0;
            }
            break;
        }
        default:
            break;
    }

    return status;

}


static int __init kdai_init(void) {
    /* Initialize arp netfilter hook */
    arpho = (struct nf_hook_ops *) kcalloc(1, sizeof(struct nf_hook_ops), GFP_KERNEL);
    arpho->hook = (nf_hookfn *) arp_hook;       /* hook function */
    arpho->hooknum = NF_ARP_IN;                 /* received packets */
    arpho->pf = NFPROTO_ARP;                    /* ARP */
    arpho->priority = NF_IP_PRI_FIRST;
    nf_register_hook(arpho);
    
    /* Initialize ip netfilter hook */
    ipho = (struct nf_hook_ops *) kcalloc(1, sizeof(struct nf_hook_ops), GFP_KERNEL);
    ipho->hook = (nf_hookfn *) ip_hook;         /* hook function */
    ipho->hooknum = NF_INET_PRE_ROUTING;        /* received packets */
    ipho->pf = NFPROTO_IPV4;                    /* IP */
    ipho->priority = NF_IP_PRI_FIRST;
    nf_register_hook(ipho);

    spin_lock_init(&slock);
    dhc_thread = kthread_run(dhc_th_func, NULL, "DHCP Thread");
    if(dhc_thread) {
        printk(KERN_INFO"kdai:  DHCP Thread Created Successfully...\n");
    } else {
        printk(KERN_INFO"kdai:  Cannot create kthread\n");
    }
    return 0; 
}


static void __exit kdai_exit(void) {
    nf_unregister_hook(arpho);
    kfree(arpho);
    nf_unregister_hook(ipho);
    kfree(ipho);
    clean_dhcp_snooping_table();
    kthread_stop(dhc_thread);
}

module_init(kdai_init);
module_exit(kdai_exit);