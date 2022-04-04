#ifndef _DEVICE_H
#define _DEVICE_H

extern struct MsgPort *myPort;

typedef struct cfdata *cfdata_t;

#if 0
/*
 * Configuration data (i.e., data placed in ioconf.c).
 */
struct cfdata {
    const char *cf_name;            /* driver name */
    const char *cf_atname;          /* attachment name */
    unsigned int cf_unit:24;        /* unit number */
    unsigned char cf_fstate;        /* finding state (below) */
    int     *cf_loc;                /* locators (machine dependent) */
    int     cf_flags;               /* flags from config */
    const struct cfparent *cf_pspec;/* parent specification */
};

#endif
/* Max size of a device external name (including terminating NUL) */
#define DEVICE_XNAME_SIZE       16

struct device {
                                    /* external name (name + unit) */
    char            dv_xname[DEVICE_XNAME_SIZE];
#if 0
    devhandle_t     dv_handle;      /* this device's handle;
                                       new device_t's get INVALID */
    devclass_t      dv_class;       /* this device's classification */
    TAILQ_ENTRY(device) dv_list;    /* entry on list of all devices */
    cfdata_t        dv_cfdata;      /* config data that found us
                                       (NULL if pseudo-device) */
    cfdriver_t      dv_cfdriver;    /* our cfdriver */
    cfattach_t      dv_cfattach;    /* our cfattach */
    int             dv_unit;        /* device unit number */
    device_t        dv_parent;      /* pointer to parent device
                                       (NULL if pseudo- or root node) */
    int             dv_depth;       /* number of parents until root */
    int             dv_flags;       /* misc. flags; see below */
    void            *dv_private;    /* this device's private storage */
    int             *dv_locators;   /* our actual locators (optional) */
#endif
#if 0
    prop_dictionary_t dv_properties;/* properties dictionary */

    int             dv_pending;     /* config_pending count */
    TAILQ_ENTRY(device) dv_pending_list;
#endif

#if 0
    struct lwp      *dv_detaching;  /* detach lock (config_misc_lock/cv) */

    size_t          dv_activity_count;
    void            (**dv_activity_handlers)(device_t, devactive_t);

    bool            (*dv_driver_suspend)(device_t, const pmf_qual_t *);
    bool            (*dv_driver_resume)(device_t, const pmf_qual_t *);
    bool            (*dv_driver_shutdown)(device_t, int);
    bool            (*dv_driver_child_register)(device_t);

    void            *dv_bus_private;
    bool            (*dv_bus_suspend)(device_t, const pmf_qual_t *);
    bool            (*dv_bus_resume)(device_t, const pmf_qual_t *);
    bool            (*dv_bus_shutdown)(device_t, int);
    void            (*dv_bus_deregister)(device_t);

    void            *dv_class_private;
    bool            (*dv_class_suspend)(device_t, const pmf_qual_t *);
    bool            (*dv_class_resume)(device_t, const pmf_qual_t *);
    void            (*dv_class_deregister)(device_t);

    devgen_t                dv_add_gen,
                            dv_del_gen;

    struct device_lock      dv_lock;
    const device_suspensor_t
        *dv_bus_suspensors[DEVICE_SUSPENSORS_MAX],
        *dv_driver_suspensors[DEVICE_SUSPENSORS_MAX],
        *dv_class_suspensors[DEVICE_SUSPENSORS_MAX];
    struct device_garbage dv_garbage;
#endif
};

#endif /* _DEVICE_H */
