static volatile unsigned int probe_atomic_value;

int main(void)
{
    unsigned int old_value;

    probe_atomic_value = 7u;
    old_value = __sync_val_compare_and_swap(&probe_atomic_value, 7u, 11u);
    if (old_value != 7u || probe_atomic_value != 11u) {
        return 1;
    }
    old_value = __sync_fetch_and_add(&probe_atomic_value, 5u);
    return old_value == 11u && probe_atomic_value == 16u ? 0 : 2;
}
