HXCOMM Use DEFHEADING() to define headings in both help text and texi
HXCOMM Text between STEXI and ETEXI are copied to texi version and
HXCOMM discarded from C version
HXCOMM DEF(command, callback, arg_string) is used to construct
HXCOMM command structures and help message.
HXCOMM HXCOMM can be used for comments, discarded from both texi and C

STEXI
@table @option
ETEXI

DEF("check", img_check,
    "check [-f fmt] [--output=ofmt] [-r [leaks | all]] [-T src_cache] filename")
STEXI
@item check [-f @var{fmt}] [--output=@var{ofmt}] [-r [leaks | all]] [-T @var{src_cache}] @var{filename}
ETEXI

DEF("create", img_create,
    "create [-f fmt] [-o options] filename [size]")
STEXI
@item create [-f @var{fmt}] [-o @var{options}] @var{filename} [@var{size}]
ETEXI

DEF("commit", img_commit,
    "commit [-f fmt] [-t cache] filename")
STEXI
@item commit [-f @var{fmt}] [-t @var{cache}] @var{filename}
ETEXI

DEF("compare", img_compare,
    "compare [-f fmt] [-F fmt] [-T src_cache] [-p] [-s] filename1 filename2")
STEXI
@item compare [-f @var{fmt}] [-F @var{fmt}] [-T @var{src_cache}] [-p] [-s] @var{filename1} @var{filename2}
ETEXI

DEF("convert", img_convert,
    "convert [-c] [-p] [-f fmt] [-t cache] [-T src_cache] [-O output_fmt] [-o options] [-S sparse_size] filename [filename2 [...]] output_filename")
STEXI
@item convert [-c] [-p] [-f @var{fmt}] [-t @var{cache}] [-T @var{src_cache}] [-O @var{output_fmt}] [-o @var{options}] [-S @var{sparse_size}] @var{filename} [@var{filename2} [...]] @var{output_filename}
ETEXI

DEF("info", img_info,
    "info [-f fmt] [--output=ofmt] filename")
STEXI
@item info [-f @var{fmt}] [--output=@var{ofmt}] @var{filename}
ETEXI

DEF("map", img_map,
    "map [-f fmt] [--output=ofmt] filename")
STEXI
@item map [-f @var{fmt}] [--output=@var{ofmt}] @var{filename}
ETEXI

DEF("snapshot", img_snapshot,
    "snapshot [-l | -a snapshot | -c snapshot | -d snapshot] filename")
STEXI
@item snapshot [-l | -a @var{snapshot} | -c @var{snapshot} | -d @var{snapshot}] @var{filename}
ETEXI

DEF("rebase", img_rebase,
    "rebase [-f fmt] [-t cache] [-T src_cache] [-p] [-u] -b backing_file [-F backing_fmt] filename")
STEXI
@item rebase [-f @var{fmt}] [-t @var{cache}] [-T @var{src_cache}] [-p] [-u] -b @var{backing_file} [-F @var{backing_fmt}] @var{filename}
ETEXI

DEF("resize", img_resize,
    "resize filename [+ | -]size")
STEXI
@item resize @var{filename} [+ | -]@var{size}
@end table
ETEXI
