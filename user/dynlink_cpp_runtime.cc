extern "C" int puts(const char*);

struct DynBase {
    virtual int value() const {
        return 7;
    }
};

struct DynGlobal {
    DynGlobal() : value(35) {}
    int value;
};

static DynGlobal global_state;

extern "C" int dyn_cpp_probe(void) {
    DynBase base;
    return base.value() + global_state.value;
}
