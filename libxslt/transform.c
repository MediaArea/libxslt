/*
 * transform.c: Implemetation of the XSL Transformation 1.0 engine
 *            transform part, i.e. applying a Stylesheet to a document
 *
 * Reference:
 *   http://www.w3.org/TR/1999/REC-xslt-19991116
 *
 * See Copyright for the status of this software.
 *
 * Daniel.Veillard@imag.fr
 */

#include "xsltconfig.h"

#include <string.h>

#include <libxml/xmlmemory.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/valid.h>
#include <libxml/hash.h>
#include <libxml/encoding.h>
#include <libxml/xmlerror.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>
#include <libxml/HTMLtree.h>
#include "xslt.h"
#include "xsltInternals.h"
#include "xsltutils.h"
#include "pattern.h"
#include "transform.h"
#include "variables.h"
#include "templates.h"

#define DEBUG_PROCESS

/*
 * Useful macros
 */

#define IS_BLANK_NODE(n)						\
    (((n)->type == XML_TEXT_NODE) && (xsltIsBlank((n)->content)))


/************************************************************************
 *									*
 *			
 *									*
 ************************************************************************/

/**
 * xsltNewTransformContext:
 *
 * Create a new XSLT TransformContext
 *
 * Returns the newly allocated xsltTransformContextPtr or NULL in case of error
 */
xsltTransformContextPtr
xsltNewTransformContext(void) {
    xsltTransformContextPtr cur;

    cur = (xsltTransformContextPtr) xmlMalloc(sizeof(xsltTransformContext));
    if (cur == NULL) {
        xsltGenericError(xsltGenericErrorContext,
		"xsltNewTransformContext : malloc failed\n");
	return(NULL);
    }
    memset(cur, 0, sizeof(xsltTransformContext));
    return(cur);
}

/**
 * xsltFreeTransformContext:
 * @ctxt:  an XSLT parser context
 *
 * Free up the memory allocated by @ctxt
 */
void
xsltFreeTransformContext(xsltTransformContextPtr ctxt) {
    xmlDocPtr doc, next;

    if (ctxt == NULL)
	return;
    doc = ctxt->extraDocs;
    while (doc != NULL) {
	next = (xmlDocPtr) doc->next;
	xmlFreeDoc(doc);
	doc = next;
    }
    if (ctxt->xpathCtxt != NULL)
	xmlXPathFreeContext(ctxt->xpathCtxt);
    xsltFreeVariableHashes(ctxt);
    memset(ctxt, -1, sizeof(xsltTransformContext));
    xmlFree(ctxt);
}

/************************************************************************
 *									*
 *			
 *									*
 ************************************************************************/

void xsltProcessOneNode(xsltTransformContextPtr ctxt, xmlNodePtr node);
void xsltForEach(xsltTransformContextPtr ctxt, xmlNodePtr node,
	         xmlNodePtr inst);
void xsltIf(xsltTransformContextPtr ctxt, xmlNodePtr node, xmlNodePtr inst);

/**
 * xsltSort:
 * @ctxt:  a XSLT process context
 * @node:  the node in the source tree.
 * @inst:  the xslt sort node
 *
 * Process the xslt sort node on the source node
 */
void
xsltSort(xsltTransformContextPtr ctxt, xmlNodePtr node,
	           xmlNodePtr inst) {
    xmlXPathObjectPtr *results = NULL;
    xmlNodeSetPtr list = NULL;
    xmlXPathParserContextPtr xpathParserCtxt = NULL;
    xmlChar *prop = NULL;
    xmlXPathObjectPtr res, tmp;
    const xmlChar *start;
    int descending = 0;
    int number = 0;
    int len = 0;
    int i;

    if ((ctxt == NULL) || (node == NULL) || (inst == NULL))
	return;

    list = ctxt->nodeList;
    if ((list == NULL) || (list->nodeNr <= 1))
	goto error; /* nothing to do */

    len = list->nodeNr;

    /* TODO: process attributes as attribute value templates */
    prop = xsltEvalAttrValueTemplate(ctxt, inst, (const xmlChar *)"data-type");
    if (prop != NULL) {
	if (xmlStrEqual(prop, (const xmlChar *) "text"))
	    number = 0;
	else if (xmlStrEqual(prop, (const xmlChar *) "number"))
	    number = 1;
	else {
	    xsltGenericError(xsltGenericErrorContext,
		 "xsltSort: no support for data-type = %s\n", prop);
	    goto error;
	}
	xmlFree(prop);
    }
    prop = xsltEvalAttrValueTemplate(ctxt, inst, (const xmlChar *)"order");
    if (prop != NULL) {
	if (xmlStrEqual(prop, (const xmlChar *) "ascending"))
	    descending = 0;
	else if (xmlStrEqual(prop, (const xmlChar *) "descending"))
	    descending = 1;
	else {
	    xsltGenericError(xsltGenericErrorContext,
		 "xsltSort: invalid value %s for order\n", prop);
	    goto error;
	}
	xmlFree(prop);
    }
    /* TODO: xsl:sort lang attribute */
    /* TODO: xsl:sort case-order attribute */

    prop = xmlGetNsProp(inst, (const xmlChar *)"select", XSLT_NAMESPACE);
    if (prop == NULL) {
	prop = xmlNodeGetContent(inst);
	if (prop == NULL) {
	    xsltGenericError(xsltGenericErrorContext,
		 "xsltSort: select is not defined\n");
	    return;
	}
    }

    xpathParserCtxt = xmlXPathNewParserContext(prop, ctxt->xpathCtxt);
    if (xpathParserCtxt == NULL)
	goto error;
    results = xmlMalloc(len * sizeof(xmlXPathObjectPtr));
    if (results == NULL) {
	xsltGenericError(xsltGenericErrorContext,
	     "xsltSort: memory allocation failure\n");
	goto error;
    }

    start = xpathParserCtxt->cur;
    for (i = 0;i < len;i++) {
	xpathParserCtxt->cur = start;
	node = ctxt->node = list->nodeTab[i];
	ctxt->xpathCtxt->proximityPosition = i + 1;
	valuePush(xpathParserCtxt, xmlXPathNewNodeSet(node));
	xmlXPathEvalExpr(xpathParserCtxt);
	xmlXPathStringFunction(xpathParserCtxt, 1);
	if (number)
	    xmlXPathNumberFunction(xpathParserCtxt, 1);
	res = valuePop(xpathParserCtxt);
	do {
	    tmp = valuePop(xpathParserCtxt);
	    if (tmp != NULL) {
		xmlXPathFreeObject(tmp);
	    }
	} while (tmp != NULL);

	if (res != NULL) {
	    if (number) {
		if (res->type == XPATH_NUMBER) {
		    results[i] = res;
		} else {
#ifdef DEBUG_PROCESS
		    xsltGenericDebug(xsltGenericDebugContext,
			"xsltSort: select didn't evaluate to a number\n");
#endif
		    results[i] = NULL;
		}
	    } else {
		if (res->type == XPATH_STRING) {
		    results[i] = res;
		} else {
#ifdef DEBUG_PROCESS
		    xsltGenericDebug(xsltGenericDebugContext,
			"xsltSort: select didn't evaluate to a string\n");
#endif
		    results[i] = NULL;
		}
	    }
	}
    }

    xsltSortFunction(list, &results[0], descending, number);

error:
    if (xpathParserCtxt != NULL)
	xmlXPathFreeParserContext(xpathParserCtxt);
    if (prop != NULL)
	xmlFree(prop);
    if (results != NULL) {
	for (i = 0;i < len;i++)
	    xmlXPathFreeObject(results[i]);
	xmlFree(results);
    }
}

/**
 * xsltComment:
 * @ctxt:  a XSLT process context
 * @node:  the node in the source tree.
 * @inst:  the xslt comment node
 *
 * Process the xslt comment node on the source node
 */
void
xsltComment(xsltTransformContextPtr ctxt, xmlNodePtr node,
	           xmlNodePtr inst) {
    xmlChar *value = NULL;
    xmlNodePtr comment;

    value = xsltEvalTemplateString(ctxt, node, inst);
    /* TODO: check that there is no -- sequence and doesn't end up with - */
#ifdef DEBUG_PROCESS
    if (value == NULL)
	xsltGenericDebug(xsltGenericDebugContext,
	     "xsl:comment: empty\n");
    else
	xsltGenericDebug(xsltGenericDebugContext,
	     "xsl:comment: content %s\n", value);
#endif

    comment = xmlNewComment(value);
    xmlAddChild(ctxt->insert, comment);

    if (value != NULL)
	xmlFree(value);
}

/**
 * xsltProcessingInstruction:
 * @ctxt:  a XSLT process context
 * @node:  the node in the source tree.
 * @inst:  the xslt processing-instruction node
 *
 * Process the xslt processing-instruction node on the source node
 */
void
xsltProcessingInstruction(xsltTransformContextPtr ctxt, xmlNodePtr node,
	           xmlNodePtr inst) {
    xmlChar *ncname = NULL;
    xmlChar *value = NULL;
    xmlNodePtr pi;


    if (ctxt->insert == NULL)
	return;
    ncname = xsltEvalAttrValueTemplate(ctxt, inst, (const xmlChar *)"name");
    if (ncname == NULL) {
	xsltGenericError(xsltGenericErrorContext,
	     "xslt:processing-instruction : name is missing\n");
	goto error;
    }
    /* TODO: check that it's both an an NCName and a PITarget. */


    value = xsltEvalTemplateString(ctxt, node, inst);
    /* TODO: check that there is no ?> sequence */
#ifdef DEBUG_PROCESS
    if (value == NULL)
	xsltGenericDebug(xsltGenericDebugContext,
	     "xsl:processing-instruction: %s empty\n", ncname);
    else
	xsltGenericDebug(xsltGenericDebugContext,
	     "xsl:processing-instruction: %s content %s\n", ncname, value);
#endif

    pi = xmlNewPI(ncname, value);
    xmlAddChild(ctxt->insert, pi);

error:
    if (ncname != NULL)
        xmlFree(ncname);
    if (value != NULL)
	xmlFree(value);
}

/**
 * xsltAttribute:
 * @ctxt:  a XSLT process context
 * @node:  the node in the source tree.
 * @inst:  the xslt attribute node
 *
 * Process the xslt attribute node on the source node
 */
void
xsltAttribute(xsltTransformContextPtr ctxt, xmlNodePtr node,
	           xmlNodePtr inst) {
    xmlChar *prop = NULL;
    xmlChar *ncname = NULL;
    xmlChar *prefix = NULL;
    xmlChar *value = NULL;
    xmlNsPtr ns = NULL;
    xmlAttrPtr attr;


    if (ctxt->insert == NULL)
	return;
    if (ctxt->insert->children != NULL) {
	xsltGenericError(xsltGenericErrorContext,
	     "xslt:attribute : node has already children\n");
	return;
    }
    prop = xsltEvalAttrValueTemplate(ctxt, inst, (const xmlChar *)"namespace");
    if (prop != NULL) {
	TODO /* xsl:attribute namespace */
	xmlFree(prop);
	return;
    }
    prop = xsltEvalAttrValueTemplate(ctxt, inst, (const xmlChar *)"name");
    if (prop == NULL) {
	xsltGenericError(xsltGenericErrorContext,
	     "xslt:attribute : name is missing\n");
	goto error;
    }

    ncname = xmlSplitQName2(prop, &prefix);
    if (ncname == NULL) {
	ncname = prop;
	prop = NULL;
	prefix = NULL;
    }
    if (xmlStrEqual(ncname, (const xmlChar *) "xmlns")) {
	xsltGenericError(xsltGenericErrorContext,
	     "xslt:attribute : xmlns forbidden\n");
	goto error;
    }
    if ((prefix != NULL) && (ns == NULL)) {
	ns = xmlSearchNs(ctxt->insert->doc, ctxt->insert, prefix);
	if (ns == NULL) {
	    xsltGenericError(xsltGenericErrorContext,
		"no namespace bound to prefix %s\n", prefix);
	}
    }

    value = xsltEvalTemplateString(ctxt, node, inst);
    if (value == NULL) {
	if (ns) {
	    attr = xmlSetNsProp(ctxt->insert, ns, ncname, 
		                (const xmlChar *)"");
	} else
	    attr = xmlSetProp(ctxt->insert, ncname, (const xmlChar *)"");
    } else {
	if (ns) {
	    attr = xmlSetNsProp(ctxt->insert, ns, ncname, value);
	} else
	    attr = xmlSetProp(ctxt->insert, ncname, value);
	
    }

error:
    if (prop != NULL)
        xmlFree(prop);
    if (ncname != NULL)
        xmlFree(ncname);
    if (prefix != NULL)
        xmlFree(prefix);
    if (value != NULL)
        xmlFree(value);
}

/**
 * xsltValueOf:
 * @ctxt:  a XSLT process context
 * @node:  the node in the source tree.
 * @inst:  the xsltValueOf node
 *
 * Process the xsltValueOf node on the source node
 */
void
xsltValueOf(xsltTransformContextPtr ctxt, xmlNodePtr node,
	           xmlNodePtr inst) {
    xmlChar *prop;
    int disableEscaping = 0;
    xmlXPathObjectPtr res = NULL, tmp;
    xmlXPathParserContextPtr xpathParserCtxt = NULL;
    xmlNodePtr copy = NULL;

    if ((ctxt == NULL) || (node == NULL) || (inst == NULL))
	return;

    prop = xmlGetNsProp(inst, (const xmlChar *)"disable-output-escaping",
	                XSLT_NAMESPACE);
    if (prop != NULL) {
	if (xmlStrEqual(prop, (const xmlChar *)"yes"))
	    disableEscaping = 1;
	else if (xmlStrEqual(prop, (const xmlChar *)"no"))
	    disableEscaping = 0;
	else 
	    xsltGenericError(xsltGenericErrorContext,
		 "invalud value %s for disable-output-escaping\n", prop);

	xmlFree(prop);
	if (disableEscaping) {
	    TODO /* disable-output-escaping */
	}
    }
    prop = xmlGetNsProp(inst, (const xmlChar *)"select", XSLT_NAMESPACE);
    if (prop == NULL) {
	xsltGenericError(xsltGenericErrorContext,
	     "xsltValueOf: select is not defined\n");
	return;
    }
#ifdef DEBUG_PROCESS
    xsltGenericDebug(xsltGenericDebugContext,
	 "xsltValueOf: select %s\n", prop);
#endif

    if (ctxt->xpathCtxt == NULL) {
	xmlXPathInit();
	ctxt->xpathCtxt = xmlXPathNewContext(ctxt->doc);
	if (ctxt->xpathCtxt == NULL)
	    goto error;
	XSLT_REGISTER_VARIABLE_LOOKUP(ctxt);
    }
    xpathParserCtxt =
	xmlXPathNewParserContext(prop, ctxt->xpathCtxt);
    if (xpathParserCtxt == NULL)
	goto error;
    ctxt->xpathCtxt->node = node;
    valuePush(xpathParserCtxt, xmlXPathNewNodeSet(node));
    xmlXPathEvalExpr(xpathParserCtxt);
    xmlXPathStringFunction(xpathParserCtxt, 1);
    res = valuePop(xpathParserCtxt);
    do {
        tmp = valuePop(xpathParserCtxt);
	if (tmp != NULL) {
	    xmlXPathFreeObject(tmp);
	}
    } while (tmp != NULL);
    if (res != NULL) {
	if (res->type == XPATH_STRING) {
            copy = xmlNewText(res->stringval);
	    if (copy != NULL) {
		xmlAddChild(ctxt->insert, copy);
	    }
	}
    }
    if (copy == NULL) {
	xsltGenericError(xsltGenericErrorContext,
	    "xsltDefaultProcessOneNode: text copy failed\n");
    }
#ifdef DEBUG_PROCESS
    else
	xsltGenericDebug(xsltGenericDebugContext,
	     "xsltValueOf: result %s\n", res->stringval);
#endif
error:
    if (xpathParserCtxt != NULL) {
	xmlXPathFreeParserContext(xpathParserCtxt);
        xpathParserCtxt = NULL;
    }
    if (prop != NULL)
	xmlFree(prop);
    if (res != NULL)
	xmlXPathFreeObject(res);
}

/**
 * xsltCopyNode:
 * @ctxt:  a XSLT process context
 * @node:  the element node in the source tree.
 * @insert:  the parent in the result tree.
 *
 * Make a copy of the element node @node
 * and insert it as last child of @insert
 *
 * Returns a pointer to the new node, or NULL in case of error
 */
xmlNodePtr
xsltCopyNode(xsltTransformContextPtr ctxt, xmlNodePtr node,
	     xmlNodePtr insert) {
    xmlNodePtr copy;

    copy = xmlCopyNode(node, 0);
    copy->doc = ctxt->output;
    if (copy != NULL) {
	xmlAddChild(insert, copy);
	/*
	 * Add namespaces as they are needed
	 */
	if (node->nsDef != NULL)
	    copy->nsDef = xmlCopyNamespaceList(node->nsDef);
	if (node->ns != NULL) {
	    /*
	     * optimization, if the namespace is already the
	     * on on the parent node, reuse it directly
	     *
	     * TODO: check possible mess with xmlCopyNamespaceList
	     */
	    if ((insert->type == XML_ELEMENT_NODE) &&
		(insert->ns != NULL) && 
		(xmlStrEqual(insert->ns->href, node->ns->href))) {
		copy->ns = insert->ns;
	    } else {
		xmlNsPtr ns;

		/*
		 * Look in the output tree if the namespace is
		 * already in scope.
		 */
		ns = xmlSearchNsByHref(ctxt->output, copy,
				       node->ns->href);
		if (ns != NULL)
		    copy->ns = ns;
		else {
		    ns = xmlNewNs(copy, node->ns->href,
				  node->ns->prefix);
		}
	    }
	}
    } else {
	xsltGenericError(xsltGenericErrorContext,
		"xsltCopyNode: copy %s failed\n", node->name);
    }
    return(copy);
}

/**
 * xsltDefaultProcessOneNode:
 * @ctxt:  a XSLT process context
 * @node:  the node in the source tree.
 *
 * Process the source node with the default built-in template rule:
 * <xsl:template match="*|/">
 *   <xsl:apply-templates/>
 * </xsl:template>
 *
 * and
 *
 * <xsl:template match="text()|@*">
 *   <xsl:value-of select="."/>
 * </xsl:template>
 *
 * Note also that namespaces declarations are copied directly:
 *
 * the built-in template rule is the only template rule that is applied
 * for namespace nodes.
 */
void
xsltDefaultProcessOneNode(xsltTransformContextPtr ctxt, xmlNodePtr node) {
    xmlNodePtr copy;
    xmlNodePtr delete = NULL;
    int strip_spaces = -1;

    switch (node->type) {
	case XML_DOCUMENT_NODE:
	case XML_HTML_DOCUMENT_NODE:
	case XML_ELEMENT_NODE:
	    break;
	case XML_TEXT_NODE:
	    copy = xmlCopyNode(node, 0);
	    if (copy != NULL) {
		xmlAddChild(ctxt->insert, copy);
	    } else {
		xsltGenericError(xsltGenericErrorContext,
		    "xsltDefaultProcessOneNode: text copy failed\n");
	    }
	    return;
	default:
	    return;
    }
    node = node->children;
    while (node != NULL) {
	switch (node->type) {
	    case XML_DOCUMENT_NODE:
	    case XML_HTML_DOCUMENT_NODE:
	    case XML_ELEMENT_NODE:
		xsltProcessOneNode(ctxt, node);
		break;
	    case XML_TEXT_NODE:
		/* TODO: check the whitespace stripping rules ! */
		if ((IS_BLANK_NODE(node)) &&
		    (node->parent != NULL) &&
		    (ctxt->style->stripSpaces != NULL)) {
		    const xmlChar *val;

		    if (strip_spaces == -1) {
			/* TODO: add namespaces support */
			val = (const xmlChar *)
			      xmlHashLookup(ctxt->style->stripSpaces,
					    node->parent->name);
			if (val != NULL) {
			    if (xmlStrEqual(val, (xmlChar *) "strip"))
				strip_spaces = 1;
			    if (xmlStrEqual(val, (xmlChar *) "preserve"))
				strip_spaces = 0;
			} 
			if (strip_spaces == -1) {
			    val = (const xmlChar *)
				  xmlHashLookup(ctxt->style->stripSpaces,
						(const xmlChar *)"*");
			    if ((val != NULL) &&
				(xmlStrEqual(val, (xmlChar *) "strip")))
				strip_spaces = 1;
			    else
				strip_spaces = 0;
			}
		    }
		    if (strip_spaces == 1) {
			delete = node;
			break;
		    }
		}
		/* no break on purpose */
	    case XML_CDATA_SECTION_NODE:
		copy = xmlCopyNode(node, 0);
		if (copy != NULL) {
		    xmlAddChild(ctxt->insert, copy);
		} else {
		    xsltGenericError(xsltGenericErrorContext,
			"xsltDefaultProcessOneNode: text copy failed\n");
		}
		break;
	    default:
#ifdef DEBUG_PROCESS
		xsltGenericDebug(xsltGenericDebugContext,
		 "xsltDefaultProcessOneNode: skipping node type %d\n",
		                 node->type);
#endif
		delete = node;
	}
	node = node->next;
	if (delete != NULL) {
#ifdef DEBUG_PROCESS
	    xsltGenericDebug(xsltGenericDebugContext,
		 "xsltDefaultProcessOneNode: removing ignorable blank node\n");
#endif
	    xmlUnlinkNode(delete);
	    xmlFreeNode(delete);
	    delete = NULL;
	}
    }
}

/**
 * xsltCallTemplate:
 * @ctxt:  a XSLT process context
 * @node:  the node in the source tree.
 * @inst:  the xslt call-template node
 *
 * Process the xslt call-template node on the source node
 */
void
xsltCallTemplate(xsltTransformContextPtr ctxt, xmlNodePtr node,
	           xmlNodePtr inst) {
    xmlChar *prop = NULL;
    xmlChar *ncname = NULL;
    xmlChar *prefix = NULL;
    xmlNsPtr ns = NULL;
    xsltTemplatePtr template;
    xmlNodePtr cur;
    int has_param = 0;


    if (ctxt->insert == NULL)
	return;
    prop = xmlGetNsProp(inst, (const xmlChar *)"name", XSLT_NAMESPACE);
    if (prop == NULL) {
	xsltGenericError(xsltGenericErrorContext,
	     "xslt:call-template : name is missing\n");
	goto error;
    }

    ncname = xmlSplitQName2(prop, &prefix);
    if (ncname == NULL) {
	ncname = prop;
	prop = NULL;
	prefix = NULL;
    }
    if ((prefix != NULL) && (ns == NULL)) {
	ns = xmlSearchNs(ctxt->insert->doc, ctxt->insert, prefix);
	if (ns == NULL) {
	    xsltGenericError(xsltGenericErrorContext,
		"no namespace bound to prefix %s\n", prefix);
	}
    }
    if (ns != NULL)
	template = xsltFindTemplate(ctxt->style, ncname, ns->href);
    else
	template = xsltFindTemplate(ctxt->style, ncname, NULL);
    if (template == NULL) {
	xsltGenericError(xsltGenericDebugContext,
	     "xslt:call-template: template %s not found\n", cur->name);
	goto error;
    }
    cur = inst->children;
    while (cur != NULL) {
	if (IS_XSLT_ELEM(cur)) {
	    if (IS_XSLT_NAME(cur, "with-param")) {
		if (has_param == 0) {
		    xsltPushStack(ctxt);
		    has_param = 1;
		}
		xsltParseStylesheetParam(ctxt, cur);
	    } else {
		xsltGenericError(xsltGenericDebugContext,
		     "xslt:call-template: misplaced xslt:%s\n", cur->name);
	    }
	} else {
	    xsltGenericError(xsltGenericDebugContext,
		 "xslt:call-template: misplaced %s element\n", cur->name);
	}
	cur = cur->next;
    }
    xsltApplyOneTemplate(ctxt, node, template->content);

error:
    if (has_param == 1)
	xsltPopStack(ctxt);
    if (prop != NULL)
        xmlFree(prop);
    if (ncname != NULL)
        xmlFree(ncname);
    if (prefix != NULL)
        xmlFree(prefix);
}

/**
 * xsltApplyTemplates:
 * @ctxt:  a XSLT process context
 * @node:  the node in the source tree.
 * @inst:  the apply-templates node
 *
 * Process the apply-templates node on the source node
 */
void
xsltApplyTemplates(xsltTransformContextPtr ctxt, xmlNodePtr node,
	           xmlNodePtr inst) {
    xmlChar *prop = NULL;
    xmlNodePtr cur, delete = NULL;
    xmlXPathObjectPtr res = NULL, tmp;
    xmlNodePtr replacement;
    xmlNodeSetPtr list = NULL, oldlist;
    xmlXPathParserContextPtr xpathParserCtxt = NULL;
    int i, oldProximityPosition, oldContextSize;

    if ((ctxt == NULL) || (node == NULL) || (inst == NULL))
	return;

#ifdef DEBUG_PROCESS
    xsltGenericDebug(xsltGenericDebugContext,
	 "xsltApplyTemplates: node: %s\n", node->name);
#endif
    prop = xmlGetNsProp(inst, (const xmlChar *)"select", XSLT_NAMESPACE);
    if (prop != NULL) {
#ifdef DEBUG_PROCESS
	xsltGenericDebug(xsltGenericDebugContext,
	     "xsltApplyTemplates: select %s\n", prop);
#endif

	if (ctxt->xpathCtxt == NULL) {
	    xmlXPathInit();
	    ctxt->xpathCtxt = xmlXPathNewContext(ctxt->doc);
	    if (ctxt->xpathCtxt == NULL)
		goto error;
	    XSLT_REGISTER_VARIABLE_LOOKUP(ctxt);
	}
	xpathParserCtxt =
	    xmlXPathNewParserContext(prop, ctxt->xpathCtxt);
	if (xpathParserCtxt == NULL)
	    goto error;
	ctxt->xpathCtxt->node = node;
	valuePush(xpathParserCtxt, xmlXPathNewNodeSet(node));
	xmlXPathEvalExpr(xpathParserCtxt);
	res = valuePop(xpathParserCtxt);
	do {
	    tmp = valuePop(xpathParserCtxt);
	    if (tmp != NULL) {
		xmlXPathFreeObject(tmp);
	    }
	} while (tmp != NULL);
	if (res != NULL) {
	    if (res->type == XPATH_NODESET) {
		list = res->nodesetval;
		res->nodesetval = NULL;
	     } else {
#ifdef DEBUG_PROCESS
		xsltGenericDebug(xsltGenericDebugContext,
		    "xsltApplyTemplates: select didn't evaluate to a node list\n");
#endif
		goto error;
	    }
	}
    } else {
	/*
	 * Build an XPath nodelist with the children
	 */
	list = xmlXPathNodeSetCreate(NULL);
	cur = node->children;
	while (cur != NULL) {
	    switch (cur->type) {
		case XML_TEXT_NODE:
		    /* TODO: check the whitespace stripping rules ! */
		    if ((IS_BLANK_NODE(cur)) &&
			(cur->parent != NULL) &&
			(ctxt->style->stripSpaces != NULL)) {
			const xmlChar *val;

			val = (const xmlChar *)
			      xmlHashLookup(ctxt->style->stripSpaces,
					    cur->parent->name);
			if ((val != NULL) &&
			    (xmlStrEqual(val, (xmlChar *) "strip"))) {
			    delete = cur;
			    break;
			}
		    }
		    /* no break on purpose */
		case XML_DOCUMENT_NODE:
		case XML_HTML_DOCUMENT_NODE:
		case XML_ELEMENT_NODE:
		case XML_CDATA_SECTION_NODE:
		    xmlXPathNodeSetAdd(list, cur);
		    break;
		default:
#ifdef DEBUG_PROCESS
		    xsltGenericDebug(xsltGenericDebugContext,
		     "xsltApplyTemplates: skipping cur type %d\n",
				     cur->type);
#endif
		    delete = cur;
	    }
	    cur = cur->next;
	    if (delete != NULL) {
#ifdef DEBUG_PROCESS
		xsltGenericDebug(xsltGenericDebugContext,
		     "xsltApplyTemplates: removing ignorable blank cur\n");
#endif
		xmlUnlinkNode(delete);
		xmlFreeNode(delete);
		delete = NULL;
	    }
	}
    }

#ifdef DEBUG_PROCESS
    xsltGenericDebug(xsltGenericDebugContext,
	"xsltApplyTemplates: list of %d nodes\n", list->nodeNr);
#endif

    oldlist = ctxt->nodeList;
    ctxt->nodeList = list;
    oldContextSize = ctxt->xpathCtxt->contextSize;
    oldProximityPosition = ctxt->xpathCtxt->proximityPosition;
    ctxt->xpathCtxt->contextSize = list->nodeNr;

    /* 
     * handle and skip the xsl:sort
     */
    replacement = inst->children;
    while (IS_XSLT_ELEM(replacement) && (IS_XSLT_NAME(replacement, "sort"))) {
	xsltSort(ctxt, node, replacement);
	replacement = replacement->next;
    }

    for (i = 0;i < list->nodeNr;i++) {
	ctxt->node = list->nodeTab[i];
	ctxt->xpathCtxt->proximityPosition = i + 1;
	xsltProcessOneNode(ctxt, list->nodeTab[i]);
    }
    ctxt->nodeList = oldlist;
    ctxt->xpathCtxt->contextSize = oldContextSize;
    ctxt->xpathCtxt->proximityPosition = oldProximityPosition;

error:
    if (xpathParserCtxt != NULL)
	xmlXPathFreeParserContext(xpathParserCtxt);
    if (prop != NULL)
	xmlFree(prop);
    if (res != NULL)
	xmlXPathFreeObject(res);
    if (list != NULL)
	xmlXPathFreeNodeSet(list);
}

/**
 * xsltApplyOneTemplate:
 * @ctxt:  a XSLT process context
 * @node:  the node in the source tree.
 * @list:  the template replacement nodelist
 *
 * Process the apply-templates node on the source node
 */
void
xsltApplyOneTemplate(xsltTransformContextPtr ctxt, xmlNodePtr node,
	             xmlNodePtr list) {
    xmlNodePtr cur = NULL, insert, copy = NULL;
    xmlNodePtr oldInsert;
    int has_variables = 0;

    oldInsert = insert = ctxt->insert;
    /*
     * Insert all non-XSLT nodes found in the template
     */
    cur = list;
    while (cur != NULL) {
	/*
	 * test, we must have a valid insertion point
	 */
	if (insert == NULL) {
#ifdef DEBUG_PROCESS
	    xsltGenericDebug(xsltGenericDebugContext,
		 "xsltApplyOneTemplate: insert == NULL !\n");
#endif
	    return;
	}

	if (IS_XSLT_ELEM(cur)) {
	    if (IS_XSLT_NAME(cur, "apply-templates")) {
		ctxt->insert = insert;
		xsltApplyTemplates(ctxt, node, cur);
		ctxt->insert = oldInsert;
	    } else if (IS_XSLT_NAME(cur, "value-of")) {
		ctxt->insert = insert;
		xsltValueOf(ctxt, node, cur);
		ctxt->insert = oldInsert;
	    } else if (IS_XSLT_NAME(cur, "if")) {
		ctxt->insert = insert;
		xsltIf(ctxt, node, cur);
		ctxt->insert = oldInsert;
	    } else if (IS_XSLT_NAME(cur, "for-each")) {
		ctxt->insert = insert;
		xsltForEach(ctxt, node, cur);
		ctxt->insert = oldInsert;
	    } else if (IS_XSLT_NAME(cur, "attribute")) {
		ctxt->insert = insert;
		xsltAttribute(ctxt, node, cur);
		ctxt->insert = oldInsert;
		/*******
	    } else if (IS_XSLT_NAME(cur, "element")) {
		ctxt->insert = insert;
		xsltElement(ctxt, node, cur);
		ctxt->insert = oldInsert;
		*******/
	    } else if (IS_XSLT_NAME(cur, "comment")) {
		ctxt->insert = insert;
		xsltComment(ctxt, node, cur);
		ctxt->insert = oldInsert;
	    } else if (IS_XSLT_NAME(cur, "processing-instruction")) {
		ctxt->insert = insert;
		xsltProcessingInstruction(ctxt, node, cur);
		ctxt->insert = oldInsert;
	    } else if (IS_XSLT_NAME(cur, "variable")) {
		if (has_variables == 0) {
		    xsltPushStack(ctxt);
		    has_variables = 1;
		}
		xsltParseStylesheetVariable(ctxt, cur);
	    } else if (IS_XSLT_NAME(cur, "param")) {
		if (has_variables == 0) {
		    xsltPushStack(ctxt);
		    has_variables = 1;
		}
		xsltParseStylesheetParam(ctxt, cur);
	    } else if (IS_XSLT_NAME(cur, "call-template")) {
		if (has_variables == 0) {
		    xsltPushStack(ctxt);
		    has_variables = 1;
		}
		xsltCallTemplate(ctxt, node, cur);
	    } else {
#ifdef DEBUG_PROCESS
		xsltGenericError(xsltGenericDebugContext,
		     "xsltApplyOneTemplate: found xslt:%s\n", cur->name);
#endif
		TODO
	    }
	    goto skip_children;
	} else if (cur->type == XML_TEXT_NODE) {
	    /*
	     * This text comes from the stylesheet
	     * For stylesheets, the set of whitespace-preserving
	     * element names consists of just xsl:text.
	     */
#ifdef DEBUG_PROCESS
	    xsltGenericDebug(xsltGenericDebugContext,
		 "xsltApplyOneTemplate: copy text %s\n", cur->content);
#endif
	    copy = xmlCopyNode(cur, 0);
	    if (copy != NULL) {
		xmlAddChild(insert, copy);
	    } else {
		xsltGenericError(xsltGenericErrorContext,
			"xsltApplyOneTemplate: text copy failed\n");
	    }
	} else if (cur->type == XML_ELEMENT_NODE) {
#ifdef DEBUG_PROCESS
	    xsltGenericDebug(xsltGenericDebugContext,
		 "xsltApplyOneTemplate: copy node %s\n", cur->name);
#endif
	    copy = xsltCopyNode(ctxt, cur, insert);
	    /*
	     * all the attributes are directly inherited
	     * TODO: Do the substitution of {} XPath expressions !!!
	     */
	    if (cur->properties != NULL)
		copy->properties = xsltAttrListTemplateProcess(ctxt,
			                       copy, cur->properties);
	}

	/*
	 * Skip to next node, in document order.
	 */
	if (cur->children != NULL) {
	    if (cur->children->type != XML_ENTITY_DECL) {
		cur = cur->children;
		if (copy != NULL)
		    insert = copy;
		continue;
	    }
	}
skip_children:
	if (cur->next != NULL) {
	    cur = cur->next;
	    continue;
	}
	
	do {
	    cur = cur->parent;
	    insert = insert->parent;
	    if (cur == NULL)
		break;
	    if (cur == list->parent) {
		cur = NULL;
		break;
	    }
	    if (cur->next != NULL) {
		cur = cur->next;
		break;
	    }
	} while (cur != NULL);
    }
    if (has_variables != 0) {
	xsltPopStack(ctxt);
    }
}

/**
 * xsltIf:
 * @ctxt:  a XSLT process context
 * @node:  the node in the source tree.
 * @inst:  the xslt if node
 *
 * Process the xslt if node on the source node
 */
void
xsltIf(xsltTransformContextPtr ctxt, xmlNodePtr node,
	           xmlNodePtr inst) {
    xmlChar *prop;
    xmlXPathObjectPtr res = NULL, tmp;
    xmlXPathParserContextPtr xpathParserCtxt = NULL;
    int doit = 1;

    if ((ctxt == NULL) || (node == NULL) || (inst == NULL))
	return;

    prop = xmlGetNsProp(inst, (const xmlChar *)"test", XSLT_NAMESPACE);
    if (prop == NULL) {
	xsltGenericError(xsltGenericErrorContext,
	     "xsltIf: test is not defined\n");
	return;
    }
#ifdef DEBUG_PROCESS
    xsltGenericDebug(xsltGenericDebugContext,
	 "xsltIf: test %s\n", prop);
#endif

    if (ctxt->xpathCtxt == NULL) {
	xmlXPathInit();
	ctxt->xpathCtxt = xmlXPathNewContext(ctxt->doc);
	if (ctxt->xpathCtxt == NULL)
	    goto error;
	XSLT_REGISTER_VARIABLE_LOOKUP(ctxt);
    }
    xpathParserCtxt = xmlXPathNewParserContext(prop, ctxt->xpathCtxt);
    if (xpathParserCtxt == NULL)
	goto error;
    ctxt->xpathCtxt->node = node;
    valuePush(xpathParserCtxt, xmlXPathNewNodeSet(node));
    xmlXPathEvalExpr(xpathParserCtxt);
    xmlXPathBooleanFunction(xpathParserCtxt, 1);
    res = valuePop(xpathParserCtxt);
    do {
        tmp = valuePop(xpathParserCtxt);
	if (tmp != NULL) {
	    xmlXPathFreeObject(tmp);
	}
    } while (tmp != NULL);

    if (res != NULL) {
	if (res->type == XPATH_BOOLEAN)
	    doit = res->boolval;
	else {
#ifdef DEBUG_PROCESS
	    xsltGenericDebug(xsltGenericDebugContext,
		"xsltIf: test didn't evaluate to a boolean\n");
#endif
	    goto error;
	}
    }

#ifdef DEBUG_PROCESS
    xsltGenericDebug(xsltGenericDebugContext,
	"xsltIf: test evaluate to %d\n", doit);
#endif
    if (doit) {
	xsltApplyOneTemplate(ctxt, ctxt->node, inst->children);
    }

error:
    if (xpathParserCtxt != NULL)
	xmlXPathFreeParserContext(xpathParserCtxt);
    if (prop != NULL)
	xmlFree(prop);
    if (res != NULL)
	xmlXPathFreeObject(res);
}

/**
 * xsltForEach:
 * @ctxt:  a XSLT process context
 * @node:  the node in the source tree.
 * @inst:  the xslt for-each node
 *
 * Process the xslt for-each node on the source node
 */
void
xsltForEach(xsltTransformContextPtr ctxt, xmlNodePtr node,
	           xmlNodePtr inst) {
    xmlChar *prop;
    xmlXPathObjectPtr res = NULL, tmp;
    xmlNodePtr replacement;
    xmlNodeSetPtr list = NULL, oldlist;
    xmlXPathParserContextPtr xpathParserCtxt = NULL;
    int i, oldProximityPosition, oldContextSize;

    if ((ctxt == NULL) || (node == NULL) || (inst == NULL))
	return;

    prop = xmlGetNsProp(inst, (const xmlChar *)"select", XSLT_NAMESPACE);
    if (prop == NULL) {
	xsltGenericError(xsltGenericErrorContext,
	     "xsltForEach: select is not defined\n");
	return;
    }
#ifdef DEBUG_PROCESS
    xsltGenericDebug(xsltGenericDebugContext,
	 "xsltForEach: select %s\n", prop);
#endif

    if (ctxt->xpathCtxt == NULL) {
	xmlXPathInit();
	ctxt->xpathCtxt = xmlXPathNewContext(ctxt->doc);
	if (ctxt->xpathCtxt == NULL)
	    goto error;
	XSLT_REGISTER_VARIABLE_LOOKUP(ctxt);
    }
    xpathParserCtxt = xmlXPathNewParserContext(prop, ctxt->xpathCtxt);
    if (xpathParserCtxt == NULL)
	goto error;
    ctxt->xpathCtxt->node = node;
    valuePush(xpathParserCtxt, xmlXPathNewNodeSet(node));
    xmlXPathEvalExpr(xpathParserCtxt);
    res = valuePop(xpathParserCtxt);
    do {
        tmp = valuePop(xpathParserCtxt);
	if (tmp != NULL) {
	    xmlXPathFreeObject(tmp);
	}
    } while (tmp != NULL);

    if (res != NULL) {
	if (res->type == XPATH_NODESET)
	    list = res->nodesetval;
	else {
#ifdef DEBUG_PROCESS
	    xsltGenericDebug(xsltGenericDebugContext,
		"xsltForEach: select didn't evaluate to a node list\n");
#endif
	    goto error;
	}
    }

#ifdef DEBUG_PROCESS
    xsltGenericDebug(xsltGenericDebugContext,
	"xsltForEach: select evaluate to %d nodes\n", list->nodeNr);
#endif

    oldlist = ctxt->nodeList;
    ctxt->nodeList = list;
    oldContextSize = ctxt->xpathCtxt->contextSize;
    oldProximityPosition = ctxt->xpathCtxt->proximityPosition;
    ctxt->xpathCtxt->contextSize = list->nodeNr;

    /* 
     * handle and skip the xsl:sort
     */
    replacement = inst->children;
    while (IS_XSLT_ELEM(replacement) && (IS_XSLT_NAME(replacement, "sort"))) {
	xsltSort(ctxt, node, replacement);
	replacement = replacement->next;
    }

    for (i = 0;i < list->nodeNr;i++) {
	ctxt->node = list->nodeTab[i];
	ctxt->xpathCtxt->proximityPosition = i + 1;
	xsltApplyOneTemplate(ctxt, list->nodeTab[i], replacement);
    }
    ctxt->nodeList = oldlist;
    ctxt->xpathCtxt->contextSize = oldContextSize;
    ctxt->xpathCtxt->proximityPosition = oldProximityPosition;

error:
    if (xpathParserCtxt != NULL)
	xmlXPathFreeParserContext(xpathParserCtxt);
    if (prop != NULL)
	xmlFree(prop);
    if (res != NULL)
	xmlXPathFreeObject(res);
}

/**
 * xsltProcessOneNode:
 * @ctxt:  a XSLT process context
 * @node:  the node in the source tree.
 *
 * Process the source node.
 */
void
xsltProcessOneNode(xsltTransformContextPtr ctxt, xmlNodePtr node) {
    xsltTemplatePtr template;

    template = xsltGetTemplate(ctxt->style, node);
    /*
     * If no template is found, apply the default rule.
     */
    if (template == NULL) {
#ifdef DEBUG_PROCESS
	if (node->type == XML_DOCUMENT_NODE)
	    xsltGenericDebug(xsltGenericDebugContext,
	     "xsltProcessOneNode: no template found for /\n");
	else 
	    xsltGenericDebug(xsltGenericDebugContext,
	     "xsltProcessOneNode: no template found for %s\n", node->name);
#endif
	xsltDefaultProcessOneNode(ctxt, node);
	return;
    }

    xsltApplyOneTemplate(ctxt, node, template->content);
}

/**
 * xsltApplyStylesheet:
 * @style:  a parsed XSLT stylesheet
 * @doc:  a parsed XML document
 *
 * Apply the stylesheet to the document
 * NOTE: This may lead to a non-wellformed output XML wise !
 *
 * Returns the result document or NULL in case of error
 */
xmlDocPtr
xsltApplyStylesheet(xsltStylesheetPtr style, xmlDocPtr doc) {
    xmlDocPtr res = NULL;
    xsltTransformContextPtr ctxt = NULL;
    xmlNodePtr root;

    if ((style == NULL) || (doc == NULL))
	return(NULL);
    ctxt = xsltNewTransformContext();
    if (ctxt == NULL)
	return(NULL);
    ctxt->doc = doc;
    ctxt->style = style;
    xsltEvalGlobalVariables(ctxt);
    if ((style->method != NULL) &&
	(!xmlStrEqual(style->method, (const xmlChar *) "xml"))) {
	if (xmlStrEqual(style->method, (const xmlChar *) "html")) {
	    ctxt->type = XSLT_OUTPUT_HTML;
	    res = htmlNewDoc(style->doctypePublic, style->doctypeSystem);
	    if (res == NULL)
		goto error;
	} else if (xmlStrEqual(style->method, (const xmlChar *) "text")) {
	    ctxt->type = XSLT_OUTPUT_TEXT;
	    res = xmlNewDoc(style->version);
	    if (res == NULL)
		goto error;
	} else {
	    xsltGenericError(xsltGenericErrorContext,
			     "xsltApplyStylesheet: insupported method %s\n",
		             style->method);
	    goto error;
	}
    } else {
	ctxt->type = XSLT_OUTPUT_XML;
	res = xmlNewDoc(style->version);
	if (res == NULL)
	    goto error;
    }
    res->charset = XML_CHAR_ENCODING_UTF8;
    if (style->encoding != NULL)
	res->encoding = xmlStrdup(style->encoding);

    /*
     * Start.
     */
    ctxt->output = res;
    ctxt->insert = (xmlNodePtr) res;
    ctxt->node = (xmlNodePtr) doc;
    xsltProcessOneNode(ctxt, ctxt->node);


    if ((ctxt->type = XSLT_OUTPUT_XML) &&
	((style->doctypePublic != NULL) ||
	 (style->doctypeSystem != NULL))) {
	root = xmlDocGetRootElement(res);
	if (root != NULL)
	    res->intSubset = xmlCreateIntSubset(res, root->name,
		         style->doctypePublic, style->doctypeSystem);
    }
    xmlXPathFreeNodeSet(ctxt->nodeList);
    xsltFreeTransformContext(ctxt);
    return(res);

error:
    if (res != NULL)
        xmlFreeDoc(res);
    if (ctxt != NULL)
        xsltFreeTransformContext(ctxt);
    return(NULL);
}
