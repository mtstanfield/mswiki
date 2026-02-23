# mswiki - a simple wiki for me (ms)

## goal

a very simple wiki with a simple WYSIWYG-style editor or markdown editor, similar to HedgeDoc, but with wiki-style linking.

## technical requirements

* a single statically linked binary 
* designed to be run from a distroless container, with a single volume mount for a sqlite database and loading any customization (logos and styles.
* sqlite for database (configuration, wiki page metadata, images). should be stored in a way to make it easy to recover pages and related data directly from the database using a tool like sqliteviewer
* page data stored as markdown files in database. all other required data about a page is stored in database
* minimal (preferably zero) javascript
* intended to have a reverse proxy in front of it, so no TLS or certificate handling requirements
* in C++ using this version of the NASA cody standards - https://nasa.github.io/fprime/UsersGuide/dev/code-style.html
* as low of a memory footprint as possible. as small of a binary as possible. low cpu usage
* mediawiki as the wiki model