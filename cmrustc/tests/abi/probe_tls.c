static __thread int probe_tls_value;

int main(void)
{
    probe_tls_value = 41;
    probe_tls_value += 1;
    return probe_tls_value == 42 ? 0 : 1;
}
