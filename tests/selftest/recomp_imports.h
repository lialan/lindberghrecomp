/* Stand-in for the per-game generated file, so the runtime builds standalone. */
#define HLE_IMPORTS(X) \
    X(HLE_write, "write") \
    X(HLE_glClear, "glClear") \
    X(HLE_posix_memalign, "posix_memalign") \
    X(HLE_free, "free") \
    X(HLE___cxa_vec_new2, "__cxa_vec_new2") \
    X(HLE___cxa_vec_delete2, "__cxa_vec_delete2") \
    X(HLE_list_hook, "_ZNSt15_List_node_base4hookEPS_") \
    X(HLE_list_unhook, "_ZNSt15_List_node_base6unhookEv") \
    X(HLE_rb_insert, "_ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_") \
    X(HLE_rb_erase, "_ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_") \
    X(HLE_snd_pcm_open, "snd_pcm_open") \
    X(HLE_snd_pcm_close, "snd_pcm_close") \
    X(HLE_snd_pcm_hw_params_malloc, "snd_pcm_hw_params_malloc") \
    X(HLE_snd_pcm_hw_params_free, "snd_pcm_hw_params_free") \
    X(HLE_snd_pcm_hw_params_any, "snd_pcm_hw_params_any") \
    X(HLE_snd_pcm_hw_params_set_channels, "snd_pcm_hw_params_set_channels") \
    X(HLE_snd_pcm_hw_params, "snd_pcm_hw_params") \
    X(HLE_snd_pcm_start, "snd_pcm_start") \
    X(HLE_snd_pcm_drop, "snd_pcm_drop") \
    X(HLE_snd_pcm_avail_update, "snd_pcm_avail_update") \
    X(HLE_snd_pcm_writei, "snd_pcm_writei") \
    X(HLE_sprintf, "sprintf") \
    X(HLE_test_solib_function, "test_solib_function")
