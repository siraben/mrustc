int probe_weak_value(void);

int probe_weak_value(void)
{
    return 29;
}

int main(void)
{
    return probe_weak_value() == 29 ? 0 : 1;
}
