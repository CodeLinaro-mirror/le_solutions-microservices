# CreateEvalResponsesRunDataSourceInputMessagesOneOfTemplateInner


## Properties

Name | Type | Description | Notes
------------ | ------------- | ------------- | -------------
**role** | **str** | The role of the message input. One of &#x60;user&#x60;, &#x60;assistant&#x60;, &#x60;system&#x60;, or &#x60;developer&#x60;.  | 
**content** | [**EvalItemContent**](EvalItemContent.md) |  | 
**type** | **str** | The type of the message input. Always &#x60;message&#x60;.  | [optional] 

## Example

```python
from openapi_client.models.create_eval_responses_run_data_source_input_messages_one_of_template_inner import CreateEvalResponsesRunDataSourceInputMessagesOneOfTemplateInner

# TODO update the JSON string below
json = "{}"
# create an instance of CreateEvalResponsesRunDataSourceInputMessagesOneOfTemplateInner from a JSON string
create_eval_responses_run_data_source_input_messages_one_of_template_inner_instance = CreateEvalResponsesRunDataSourceInputMessagesOneOfTemplateInner.from_json(json)
# print the JSON string representation of the object
print(CreateEvalResponsesRunDataSourceInputMessagesOneOfTemplateInner.to_json())

# convert the object into a dict
create_eval_responses_run_data_source_input_messages_one_of_template_inner_dict = create_eval_responses_run_data_source_input_messages_one_of_template_inner_instance.to_dict()
# create an instance of CreateEvalResponsesRunDataSourceInputMessagesOneOfTemplateInner from a dict
create_eval_responses_run_data_source_input_messages_one_of_template_inner_from_dict = CreateEvalResponsesRunDataSourceInputMessagesOneOfTemplateInner.from_dict(create_eval_responses_run_data_source_input_messages_one_of_template_inner_dict)
```
[[Back to Model list]](../README.md#documentation-for-models) [[Back to API list]](../README.md#documentation-for-api-endpoints) [[Back to README]](../README.md)


