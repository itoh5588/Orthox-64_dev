extern int dyn_b_total(void);

int dyn_plugin_total(void) {
    return dyn_b_total() + 1000;
}
