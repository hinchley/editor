; symbols of markdown (see ARCHITECTURE.md, "Symbols"): the headings.

(atx_heading heading_content: (_) @name) @definition.heading

(setext_heading
  heading_content: (_) @name
  [(setext_h1_underline) (setext_h2_underline)] @body) @definition.heading
