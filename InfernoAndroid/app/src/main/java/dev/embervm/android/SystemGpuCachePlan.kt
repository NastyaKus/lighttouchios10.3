// SPDX-License-Identifier: GPL-3.0-or-later
// Identity metadata, our software replacement bytes and hashes; no stock Apple code.
package dev.embervm.android

internal object SystemGpuCachePlan {
    data class Site(val offset:Long,val software:String,val originalSha256:String,val restore:Boolean)
    val sites=listOf(
        Site(0x1d435c4,"00008052","b5b2271f7ac63bb3e8bda7bd86321f8f47e1f11565615786cc0eff39933d40fb",false),
        Site(0x1d435c8,"c0035fd6","f29fb5887e2eb991d4e7be54d62d078b4c5d6a88d6e6e9463b5dabe5eb3b4ae5",false),
        Site(0x1d436cc,"00008052","b5b2271f7ac63bb3e8bda7bd86321f8f47e1f11565615786cc0eff39933d40fb",false),
        Site(0x1d436d0,"c0035fd6","f29fb5887e2eb991d4e7be54d62d078b4c5d6a88d6e6e9463b5dabe5eb3b4ae5",false),
        Site(0x1d43750,"00008052","b5b2271f7ac63bb3e8bda7bd86321f8f47e1f11565615786cc0eff39933d40fb",false),
        Site(0x1d43754,"c0035fd6","f29fb5887e2eb991d4e7be54d62d078b4c5d6a88d6e6e9463b5dabe5eb3b4ae5",false),
        Site(0x1d43858,"00008052","b5b2271f7ac63bb3e8bda7bd86321f8f47e1f11565615786cc0eff39933d40fb",false),
        Site(0x1d4385c,"c0035fd6","f29fb5887e2eb991d4e7be54d62d078b4c5d6a88d6e6e9463b5dabe5eb3b4ae5",false),
        Site(0x1d43960,"00008052","b5b2271f7ac63bb3e8bda7bd86321f8f47e1f11565615786cc0eff39933d40fb",false),
        Site(0x1d43964,"c0035fd6","f29fb5887e2eb991d4e7be54d62d078b4c5d6a88d6e6e9463b5dabe5eb3b4ae5",false),
        Site(0x1d439e4,"00008052","b5b2271f7ac63bb3e8bda7bd86321f8f47e1f11565615786cc0eff39933d40fb",false),
        Site(0x1d439e8,"c0035fd6","f29fb5887e2eb991d4e7be54d62d078b4c5d6a88d6e6e9463b5dabe5eb3b4ae5",false),
        Site(0x1d43a68,"00008052","b5b2271f7ac63bb3e8bda7bd86321f8f47e1f11565615786cc0eff39933d40fb",false),
        Site(0x1d43a6c,"c0035fd6","f29fb5887e2eb991d4e7be54d62d078b4c5d6a88d6e6e9463b5dabe5eb3b4ae5",false),
        Site(0x1e8f704,"00008052","6f048efdbe0ccddd5bae54c2cc2a016eefe51de703c28bfa80993f8e1f90c5aa",false),
        Site(0x1e8f708,"c0035fd6","110f46b5b35c069160560c6ad6786f647dd44e8760a52a46fc22dbbcd7630b91",false),
        Site(0x8143594,"1f2003d5","9a62da00306391b8e39b07481c87a3137c7705a96c9c7718d4f70a24365e1b00",false),
        Site(0x8143598,"1f2003d5","d807aeea8093a15c7af975fe60985ced9351cb99a1c65e5b250614d3e97cb6f2",false),
        Site(0x814359c,"1f2003d5","33573fb99558f1bdcc666fd68e96a4a346b080adf0682e3ef7f7096a5f3ddbad",false),
        Site(0x81435a8,"1f2003d5","8b1c9b5509827dc96638293f232ddd284771571669c011bcf99898f719ccd676",false),
        Site(0x81435d0,"000080d2","b5b2271f7ac63bb3e8bda7bd86321f8f47e1f11565615786cc0eff39933d40fb",false),
        Site(0x81435d4,"c0035fd6","e76fca0767fc7a8cc5176b0992e0338af2261280f87057457462540690d7a16e",false),
        Site(0x814395c,"1f2003d5","9a62da00306391b8e39b07481c87a3137c7705a96c9c7718d4f70a24365e1b00",false),
        Site(0x8143960,"1f2003d5","d807aeea8093a15c7af975fe60985ced9351cb99a1c65e5b250614d3e97cb6f2",false),
        Site(0x8143964,"1f2003d5","743808b96c23feadb52c45369f39dc105a6cf2c4a6717d573eb151312c5d3e21",false),
        Site(0x8143970,"1f2003d5","8b1c9b5509827dc96638293f232ddd284771571669c011bcf99898f719ccd676",false),
        Site(0x8158bdc,"000080d2","b5b2271f7ac63bb3e8bda7bd86321f8f47e1f11565615786cc0eff39933d40fb",true),
        Site(0x8158be0,"c0035fd6","9578e722f9ab0a18626e241133188c8f9463e9e1cb7297fec82fd6f65e1a90cf",true),
        Site(0x825bf10,"c0035fd6","b5b2271f7ac63bb3e8bda7bd86321f8f47e1f11565615786cc0eff39933d40fb",false),
        Site(0x12838aa4,"c0035fd6","b5b2271f7ac63bb3e8bda7bd86321f8f47e1f11565615786cc0eff39933d40fb",false),
        Site(0x15e94f18,"20008052","b5b2271f7ac63bb3e8bda7bd86321f8f47e1f11565615786cc0eff39933d40fb",false),
        Site(0x15e94f1c,"c0035fd6","3f07c89f3e39893d4731c5865d01df58e36cb6a0787414cf6d4da7d46a6f08b8",false),
        Site(0x328be438,"000080d2","b5b2271f7ac63bb3e8bda7bd86321f8f47e1f11565615786cc0eff39933d40fb",false),
        Site(0x328be43c,"c0035fd6","ea259d0539ff7cbc441385651395847efb604ac9e325d73b0e79caa83b76700a",false),
        Site(0x371ad774,"a0b91390","b5b2271f7ac63bb3e8bda7bd86321f8f47e1f11565615786cc0eff39933d40fb",false),
        Site(0x371ad778,"00202591","63b368cdb6ab9ee53a7cd531602553504c8573b654a5d4e620add844a006e9f4",false),
        Site(0x371ad77c,"2180e4f0","a4c29a61a70c921a3c7c51b708690e00238cde2cc117652c39ddb6691eefbb8f",false),
        Site(0x371ad780,"21800b91","728ba682e02aaaa234784eee6aecb2401db1fab95ba1553a3f9b19d9064a3171",false),
        Site(0x371ad784,"20003fd6","33852e83562d5496024a6067c7472dc8fbffbc52aab06ac0c34926c78beac6df",false),
        Site(0x371ad788,"c0035fd6","a744ac43d3f513a415c4d2a44b707bdac2a877873bc98d7eeae3d772671b1d2a",false),
        Site(0x427dcfcc,"c0035fd6","c6aeef4904b1a8fa4d188c9dfbb1a5fe55a0d12799de57cec735ace9a6d8baa8",false),
        Site(0x427dd614,"c0035fd6","f0005fb8188054ead417e9e237af16dad3e7dafe66cd85247bc78fc8864deb57",false),
        Site(0x59e6ac6c,"00000000","df3f619804a92fdb4057192dc43dd748ea778adc52bc498ce80524c014b81119",false),
        Site(0x59e6ac70,"00000000","df3f619804a92fdb4057192dc43dd748ea778adc52bc498ce80524c014b81119",false),
        Site(0x59e6ac74,"00000000","df3f619804a92fdb4057192dc43dd748ea778adc52bc498ce80524c014b81119",false)
    )
}
