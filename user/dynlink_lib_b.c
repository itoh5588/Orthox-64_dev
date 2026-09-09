extern int dyn_a_answer(void);

__thread int dyn_b_tls = 1;

int dyn_b_total(void) {
    dyn_b_tls += 1;
    return dyn_a_answer() + dyn_b_tls;
}
