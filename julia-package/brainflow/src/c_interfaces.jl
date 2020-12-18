using Pkg
using Pkg.Artifacts
using SHA
using Tar

function download_brainflow_artifact()

    url = brainflow_url()

    download_path = "$(tempname())-download.tar"
    download(url, download_path)

    brainflow_hash = create_artifact() do artifact_dir 
        # Pkg.PlatformEngines.unpack() gives errors for some users on Windows
        Tar.extract(download_path, artifact_dir)
    end

    rm(download_path)

    return brainflow_hash
end

function get_brainflow_artifact_path()
    return ensure_artifact_installed("brainflow", "Artifacts.toml", verbose=true)
    # try
    #     return artifact"brainflow"
    # catch
    #     @warn "Could not automatically retrieve brainflow artifact, attempting manual download"
    #     # we have a known issue with unpack(), so wrote a custom download
    #     # https://discourse.julialang.org/t/unable-to-automatically-install-artifact/51984/2
    #     download_brainflow_artifact()
    #     return artifact"brainflow"
    # end
end

function interface_path(library::AbstractString)
    return abspath(INTERFACE_PATH, interface_name(library))
end

function interface_name(library::AbstractString)
    if Sys.iswindows()
        return "$library.dll"
    elseif Sys.isapple()
        return "lib$library.dylib"
    else
        return "lib$library.so"
    end
end

# We can later replace this with:
# using Pkg.Artifacts
# const INTERFACE_PATH = artifact"brainflow"
# Right now using a hardcoded path during refactoring, with manual /lib location
#const INTERFACE_PATH = abspath(joinpath(@__DIR__, "../lib"))
const INTERFACE_PATH = get_brainflow_artifact_path()

# we should instead use dlopen(interface_path())?
const DATA_HANDLER_INTERFACE = interface_path("DataHandler")
const BOARD_CONTROLLER_INTERFACE = interface_path("BoardController")
const ML_MODULE_INTERFACE = interface_path("MLModule")
