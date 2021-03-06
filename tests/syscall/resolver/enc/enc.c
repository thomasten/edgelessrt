/* Copyright (c) Open Enclave SDK contributors. */
/* Licensed under the MIT License. */

#include <openenclave/enclave.h>
#include <openenclave/internal/time.h>

#include <openenclave/internal/syscall/arpa/inet.h>
#include <openenclave/internal/syscall/netdb.h>
#include <openenclave/internal/syscall/netinet/in.h>
#include <openenclave/internal/tests.h>

#include <resolver_test_t.h>
#include <stdio.h>
#include <string.h>
#include "../utils.h"

size_t oe_debug_malloc_check();

struct addrinfo;

static bool _initialized;

int ecall_device_init()
{
    OE_TEST(oe_load_module_host_resolver() == OE_OK);
    _initialized = true;
    return 0;
}

int ecall_getnameinfo(char* buffer, size_t bufflen)
{
    int status = OE_FAILURE;
    (void)buffer;
    (void)bufflen;
    (void)status;

    char host[256] = {0};
    char serv[256] = {0};

    struct oe_sockaddr_in addr = {
        .sin_family = OE_AF_INET,
        .sin_port = oe_htons(23), // telnet
        .sin_addr.s_addr = oe_htonl(OE_INADDR_LOOPBACK)};

    printf("s_addr=%x\n", addr.sin_addr.s_addr);

    int rslt = oe_getnameinfo(
        (const struct oe_sockaddr*)&addr,
        sizeof(addr),
        host,
        sizeof(host),
        serv,
        sizeof(serv),
        0);

    OE_TEST(rslt == 0);
    OE_TEST(strcmp(host, "127.0.0.1") == 0);
    OE_TEST(strcmp(serv, "23") == 0);

    strlcpy(buffer, host, bufflen);

    *host = 0;
    *serv = 0;

    // test with NI_NAMEREQD
    rslt = oe_getnameinfo(
        (const struct oe_sockaddr*)&addr,
        sizeof(addr),
        host,
        sizeof(host),
        serv,
        sizeof(serv),
        OE_NI_NAMEREQD);

    if (_initialized)
    {
        // expect that it can be resolved by the host
        OE_TEST(rslt == 0);
        OE_TEST(*host);
        OE_TEST(*serv);
    }
    else
        // with NI_NAMEREQD it cannot be resolved internally
        OE_TEST(rslt == OE_EAI_SYSTEM);

    return 0;
}

static void _free_addrinfo(struct oe_addrinfo* res)
{
    struct oe_addrinfo* p;

    for (p = res; p;)
    {
        struct oe_addrinfo* next = p->ai_next;

        oe_host_free(p->ai_addr);
        oe_host_free(p->ai_canonname);
        oe_host_free(p);

        p = next;
    }
}

static struct oe_addrinfo* _clone_one_addrinfo(const struct oe_addrinfo* ai)
{
    struct oe_addrinfo* ret = NULL;
    struct oe_addrinfo* p;

    /* Clone the base structure. */
    {
        if (!(p = oe_host_calloc(1, sizeof(struct oe_addrinfo))))
            goto done;

        p->ai_flags = ai->ai_flags;
        p->ai_family = ai->ai_family;
        p->ai_socktype = ai->ai_socktype;
        p->ai_protocol = ai->ai_protocol;
        p->ai_addrlen = ai->ai_addrlen;
    }

    /* Clone the ai_addrlen field. */
    if (ai->ai_addr && ai->ai_addrlen)
    {
        if (!(p->ai_addr = oe_host_calloc(1, ai->ai_addrlen)))
            goto done;

        memcpy(p->ai_addr, ai->ai_addr, ai->ai_addrlen);
    }

    /* Clone the ai_canonname field. */
    if (ai->ai_canonname)
    {
        size_t size = strlen(ai->ai_canonname) + 1;

        if (size)
        {
            if (!(p->ai_canonname = oe_host_calloc(1, size)))
                goto done;

            memcpy(p->ai_canonname, ai->ai_canonname, size);
        }
    }

    ret = p;
    p = NULL;

done:

    if (p && p->ai_addr)
        oe_host_free(p->ai_addr);

    if (p && p->ai_canonname)
        oe_host_free(p->ai_canonname);

    if (p)
        oe_host_free(p);

    return ret;
}

static struct oe_addrinfo* _clone_addrinfo(const struct oe_addrinfo* ai)
{
    struct oe_addrinfo* ret = NULL;
    struct oe_addrinfo* head = NULL;
    struct oe_addrinfo* tail = NULL;
    const struct oe_addrinfo* p;

    for (p = ai; p; p = p->ai_next)
    {
        struct oe_addrinfo* new;

        if (!(new = _clone_one_addrinfo(p)))
            goto done;

        if (tail)
        {
            tail->ai_next = new;
            tail = new;
        }
        else
        {
            head = new;
            tail = new;
        }
    }

    ret = head;
    head = NULL;
    tail = NULL;

done:

    if (head)
        _free_addrinfo(head);

    return ret;
}

int ecall_getaddrinfo(struct oe_addrinfo** res)
{
    struct oe_addrinfo* ai = NULL;
    const char host[] = {
        "lOcalhOst"}; // capital letters force external resolution
    const char serv[] = {"telnet"};
    struct oe_addrinfo hints;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (res)
        *res = NULL;

    OE_TEST(oe_getaddrinfo(host, serv, &hints, (struct oe_addrinfo**)&ai) == 0);

    if (res && !(*res = (struct oe_addrinfo*)_clone_addrinfo(ai)))
        OE_TEST("_clone_addrinfo() failed" == NULL);

    oe_freeaddrinfo(ai);

    return 0;
}

void ecall_getaddrinfo_internally(void)
{
    struct oe_addrinfo* ai = NULL;

    OE_TEST(oe_getaddrinfo("2.3.4.5", "1111", NULL, &ai) == 0);
    OE_TEST(ai && ai->ai_addr);
    OE_TEST(ai->ai_addr->sa_family == AF_INET);
    const struct oe_sockaddr_in* ad = (struct oe_sockaddr_in*)ai->ai_addr;
    OE_TEST(ntohl(ad->sin_addr.s_addr) == 0x02030405);
    OE_TEST(ntohs(ad->sin_port) == 1111);
    oe_freeaddrinfo(ai);

    // IP addresses should never be resolved by the host.
    // Named services cannot be resolved internally so the following fails.
    // (We may implement internal service name resolution if we ever need it.)
    ai = NULL;
    OE_TEST(oe_getaddrinfo("2.3.4.5", "telnet", NULL, &ai) == OE_EAI_SERVICE);
    OE_TEST(!ai);

    // localhost can be resolved internally
    ai = NULL;
    OE_TEST(oe_getaddrinfo("localhost", "1111", NULL, &ai) == 0);
    OE_TEST(ai && ai->ai_addr);
    OE_TEST(ai->ai_addr->sa_family == AF_INET);
    ad = (struct oe_sockaddr_in*)ai->ai_addr;
    OE_TEST(ntohl(ad->sin_addr.s_addr) == OE_INADDR_LOOPBACK);
    OE_TEST(ntohs(ad->sin_port) == 1111);
    oe_freeaddrinfo(ai);
}

OE_SET_ENCLAVE_SGX(
    1,    /* ProductID */
    1,    /* SecurityVersion */
    true, /* Debug */
    256,  /* NumHeapPages */
    256,  /* NumStackPages */
    1);   /* NumTCS */
