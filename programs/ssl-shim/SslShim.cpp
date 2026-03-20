extern int bssl_shim_main(int argc, char ** argv);

int mainEntryClickHouseSslShim(int argc, char ** argv)
{
    return bssl_shim_main(argc, argv);
}
