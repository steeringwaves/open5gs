# open5gs modifications

- `Attach complete`
- `Removed Session: UE` or `MME_SESS_CLEAR`


- `src/mme/emm-sm.c` -> `ogs_info("[%s] Attach complete", mme_ue->imsi_bcd);` should also send to some udp ip/port in json we also will want imei from mme_ue->imeisv
- `/src/smf/gn-handler.c` line 306 `ogs_info("UE IMSI[%s] APN[%s] IPv4[%s] IPv6[%s]",` should also emit
- 

```c

diagnostic_broadcast("{\"Command\":\"MME Initialize\"}");
diagnostic_broadcast("{\"Command\":\"MME Terminate\"}");



    diagnostic_broadcast("{\"Command\":\"Session Create\",\"IMSI\":\"%s\", \"APN\":\"%s\", \"IPv4\":\"%s\", \"IPv6\":\"%s\"}",
        smf_ue->imsi_bcd,
        sess->session.name,
        sess->ipv4 ? OGS_INET_NTOP(&sess->ipv4->addr, buf1) : "",
        sess->ipv6 ? OGS_INET6_NTOP(&sess->ipv6->addr, buf2) : "");

    diagnostic_broadcast("{\"Command\":\"Session Create\",\"IMSI\":\"%s\", \"APN\":\"%s\", \"IPv4\":\"%s\", \"IPv6\":\"%s\"}",
        smf_ue->imsi_bcd,
        sess->session.name,
        sess->ipv4 ? OGS_INET_NTOP(&sess->ipv4->addr, buf1) : "",
        sess->ipv6 ? OGS_INET6_NTOP(&sess->ipv6->addr, buf2) : "");

    diagnostic_broadcast("{\"Command\":\"Session Create\",\"IMSI\":\"%s\",\"SUPI\":\"%s\",\"IMEI\":\"%s\",\"APN\":\"%s\", \"IPv4\":\"%s\", \"IPv6\":\"%s\"}",
        smf_ue->imsi_bcd, smf_ue->supi, smf_ue->imeisv_bcd,
        sess->session.name,
        sess->ipv4 ? OGS_INET_NTOP(&sess->ipv4->addr, buf1) : "",
        sess->ipv6 ? OGS_INET6_NTOP(&sess->ipv6->addr, buf2) : "");

	diagnostic_broadcast("{\"Command\":\"Session Remove\",\"IMSI\":\"%s\", \"APN\":\"%s\"}", \
		mme_ue->imsi_bcd, \
		(__sESS)->session ? (__sESS)->session->name : "Unknown"); \

    diagnostic_broadcast("{\"Command\":\"Session Remove\",\"IMSI\":\"%s\", \"APN\":\"%s\", \"Index\":\"%d\", \"IPv4\":\"%s\", \"IPv6\":\"%s\"}",
        smf_ue->supi ? smf_ue->supi : smf_ue->imsi_bcd,
        sess->session.name, sess->psi,
        sess->ipv4 ? OGS_INET_NTOP(&sess->ipv4->addr, buf1) : "",
        sess->ipv6 ? OGS_INET6_NTOP(&sess->ipv6->addr, buf2) : "");

// UE Attach/Release
diagnostic_broadcast("{\"Command\":\"UE Attach\",\"IMSI\":\"%s\", \"IMEI\":\"%s\"}",
	smf_ue->imsi_bcd,
	mme_ue->imeisv_bcd);

diagnostic_broadcast("{\"Command\":\"UE Release\",\"IMSI\":\"%s\"}", mme_ue->imsi_bcd);
diagnostic_broadcast("{\"Command\":\"UE Release\",\"SUCI\":\"%s\",\"IMEI\":\"%s\"}", amf_ue->suci ? amf_ue->suci : "Unknown", amf_ue->imeisv_bcd);

// eNB
diagnostic_broadcast("{\"Command\":\"eNB Connect\",\"Address\":\"%s\"}", OGS_ADDR(enb->sctp.addr, buf));

char buf[OGS_ADDRSTRLEN];
diagnostic_broadcast("{\"Command\":\"eNB Disconnect\",\"Address\":\"%s\"}", OGS_ADDR(enb->sctp.addr, buf));

// gNB

diagnostic_broadcast("{\"Command\":\"gNB Connect\",\"Address\":\"%s\"}", OGS_ADDR(gnb->sctp.addr, buf));

char buf[OGS_ADDRSTRLEN];
diagnostic_broadcast("{\"Command\":\"gNB Disconnect\",\"Address\":\"%s\"}", OGS_ADDR(gnb->sctp.addr, buf));


```
