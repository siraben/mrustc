__attribute__((section(".cmrustc_probe_data")))
static int probe_section_value = 17;

__attribute__((section(".cmrustc_probe_text")))
static int probe_section_function(void)
{
    return probe_section_value;
}

int main(void)
{
    return probe_section_function() == 17 ? 0 : 1;
}
