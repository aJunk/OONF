# set library parameters
SET (name plugin_controller)

# use generic plugin maker
oonf_create_plugin("${name}" "${name}.c" "${name}.h" "")
