package org.rstudio.studio.client.panmirror.dialogs;

import org.rstudio.core.client.Debug;
import org.rstudio.core.client.widget.ModalDialog;
import org.rstudio.studio.client.panmirror.Panmirror;
import org.rstudio.studio.client.panmirror.PanmirrorConfig;
import org.rstudio.studio.client.panmirror.PanmirrorKeybindings;
import org.rstudio.studio.client.panmirror.PanmirrorWidget;
import org.rstudio.studio.client.panmirror.pandoc.PanmirrorPandocExtensions;

import com.google.gwt.aria.client.Roles;
import com.google.gwt.event.dom.client.ChangeEvent;
import com.google.gwt.event.dom.client.ChangeHandler;
import com.google.gwt.user.client.ui.SimplePanel;

import com.google.gwt.user.client.ui.Widget;
import com.google.gwt.view.client.SelectionChangeEvent;


public class PanmirrorTestDialog extends ModalDialog<String>
{
   public PanmirrorTestDialog()
   {
      super("Panmirror", Roles.getDialogRole(), input -> {});
      
      mainWidget_ = new SimplePanel();
      mainWidget_.setSize("500px", "400px");
      
      PanmirrorConfig config = new PanmirrorConfig();
      config.options.rmdCodeChunks = true;
      config.hooks.isEditable = () -> true;
     
      
      PanmirrorWidget.Options options = new PanmirrorWidget.Options();
      options.toolbar = true;
      
      PanmirrorWidget.create(config, options, editorWidget -> {
         if (editorWidget != null) {
            
            this.editorWidget_ = editorWidget;
            mainWidget_.add(this.editorWidget_);
            
            PanmirrorKeybindings keys = new PanmirrorKeybindings();
            keys.add(Panmirror.EditorCommands.Strong, new String[]{"Mod-5"});
            this.editorWidget_.setKeybindings(keys);
            
            this.editorWidget_.addChangeHandler(new ChangeHandler()
            {
               @Override
               public void onChange(ChangeEvent event)
               {
                  Debug.logToConsole("Change");
               }
            }); 
            
            this.editorWidget_.addSelectionChangeHandler(new SelectionChangeEvent.Handler()
            {
               @Override
               public void onSelectionChange(SelectionChangeEvent event)
               {
                 Debug.logToConsole("SelectionChange"); 
               }
            }); 
              
            
            
            this.editorWidget_.setMarkdown(
                  "## Heading 1\n\nThe *quick* brown **fox** jumped over the lazy dog\n\n" +
                  "## Heading 2\n\nThis is the `second` section.",
                  true, (success) -> {
               if (success) 
               {  
                  
                  this.editorWidget_.execCommand(Panmirror.EditorCommands.SelectAll);
                  
                  this.editorWidget_.getMarkdown(markdown -> {
                     Debug.logToConsole(markdown);
                  });
                  
                  PanmirrorPandocExtensions extensions = this.editorWidget_.getPandocFormat().extensions;
                  Debug.logObject(extensions);
                  
                  
                  this.editorWidget_.enableDevTools();
               }
            });
         }
      });
     
   }

   @Override
   protected String collectInput()
   {
      return "";
   }

   @Override
   protected Widget createMainWidget()
   {
      return mainWidget_;
   }

   private SimplePanel mainWidget_;
   private PanmirrorWidget editorWidget_;
}