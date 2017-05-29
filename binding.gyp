{
    "targets": [
        {
            "target_name": "sctp-addon",
            "sources": [ "./src/sctp-addon.cpp" ],
            "include_dirs": [
                "<!(node -e \"require('nan')\")"
            ],
            "libraries": [
                "-lsctp"
            ]
        }
    ]
}
