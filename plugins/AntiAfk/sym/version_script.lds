{
    global:
        Plugify_*;

        /* s2sdk binding slots (defined by s2sdk_bindings, filled by the plugify module) */
        __s2sdk_*;

        /* hudmenu binding slots (defined by hudmenu_bindings, filled by the plugify module from
           the hudmenu plugin's exported methods); pisexcompat references them. */
        __hudmenu_*;

        /* No exported methods: AntiAfk exposes nothing to other plugins. */

    local: *;
};
